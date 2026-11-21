# Intro sequence — reverse-engineering notes

Source: Ghidra decompilation of `RDR.exe` (project `_tools/ghproj`).
Full dump: [ghidra-intro-controller.log](ghidra-intro-controller.log).

## The controller: `FUN_1405F5070`

Signature `void FUN_1405F5070(longlong *state)`, called per frame while the intro
runs. It drives the legal text and the logo movies:

- Sets Flash variables by name-hash: `StartOfTransitionOut`, `LegalsCompleted`,
  `flashText1`, `goText1`, `goLogo`.
- Reads `LegalsCompleted` through the GFx table and stores the result in `cVar3`
  (`cVar3 = FUN_14017F450()`), which later chooses the transition path.
- Owns the Bink handle: `BinkClose`, `BinkSetSoundTrack(4, ...)`,
  `BinkOpen(path, 0x4004400)`, `BinkPause`, `BinkNextFrame`/`BinkWait` nearby.
- Gate: `if (1000 < (int)state[1])` — the intro only advances after a timer.

## Movie lookup

`FUN_14011A000(manager, "startscreen")` returns an index into the movie table at
`manager+0x30`; the caller then invokes vtable slot `+0xA8`. Two call sites:
`FUN_1400F63F0` and `FUN_1401EC1C0`. `DAT_142C45610` is the movie manager.

Exe strings present: `startscreen`, `pause_main`, `networking`, `playerlist`,
`LegalsCompleted`, `legals`, `mp_HUD`, `net_HUD`.

## Bink skip experiment (failed, informative)

Hook `bink2w64!BinkOpen`, return null. The caller **does** test the handle, and the
logo video disappears (screen goes black), but the intro never advances — the
controller keeps waiting for the movie/state to complete. A null handle skips the
asset, not the state machine.

`BinkOpen` is called once here, from RVA `0x5F52F7`, with the filename pointer
pointing at an in-memory `KB2` blob rather than a path string — the game feeds
Bink from memory, so filename matching is not available as a discriminator.

### What to do instead

1. Hook `BinkWait`/`BinkNextFrame`/`BinkDoFrame` to make the movie report
   completion quickly, so the controller's own state machine advances.
2. Or hook `FUN_1405F5070` and force the post-intro transition (needs the caller's
   state expectations).
3. Or find the caller that *creates* the intro state and skip creating it,
   jumping directly to the startscreen state — cleanest, needs the state machine.

## Working prototype: state-counter write

`FUN_1405F4390` (the startup wrapper) does, every frame:

```
state = *(void **)DAT_142AC5130;
n = *(int *)(state + 8);
if (n < 1001) { if (n > 0) *(int *)(state + 8) = n - 1; }
else          { FUN_1405F4CC0(); }   // transition out of the intro
```

Writing `1001` into `state+8` repeatedly (every 100 ms for the first minute) makes
the game perform that transition itself, on the main thread. Directly calling
`FUN_1405F4CC0` from a remote thread **crashes** — it must run on the game thread.

Result: the legal text and the first logo phase are skipped and the title screen is
reached around 25 s instead of ~60 s. Still one logo scene (purple R) on the way; the
counter is re-initialised per phase (`10000`, then `4` observed), so the loop has to
keep writing. A direct jump to the title state would remove the remainder — that
needs the state list behind `FUN_1405F4CC0`.

Log: [skip-test.log](skip-test.log).

## Milestone: runtime intro skip (works)

Two hooks, **no game file is modified**:

1. `bink2w64!BinkOpen` returns null — the logo movies never open.
2. `RDR.exe+0x5F5070` (phase controller) calls the original, then writes
   `state+0x11 = 1` and `state+0x14 = 0` — the phase is marked complete, which is
   the half the null-handle approach was missing (completion lives inside
   `if (param_1[6] != 0)`).

Result: title screen at ~4 s after launch (baseline ~60 s). The legal text and all
three logo phases are gone; a short copyright screen still flashes for ~1–2 s.
Game stays stable. Code: `code/probe/src/dllmain.cpp`, log: `skip-working.log`.

### The copyright flash

Suppressing the named movies through `FUN_14011A000` (return -1 for
`intro_credits`, `credits`, `legals`, `legal`) does **not** remove it, so it is not
one of those movies: it is the opening frames of `startscreen` itself. The frontend
preloads `popup`, `startscreen`, `credits`, `intro_credits`, `mp_HUD`, `loading`,
`fader`, `numberSystem` at boot (log: `skip-suppress.log`).

Publishing the flash variables the controller normally drives
(`LegalsCompleted = 1`, `flashText1`/`goText1`/`goLogo` = 0) through the engine's own
`FUN_140181220` also leaves the copyright frames intact — the movie plays them
regardless of those flags.

Dumping the movie object's vtable (`0x141C73108`) showed the `0x180xxx` slots are
movie-definition serializers (`m_FrameCount`, `m_Frames`, `swfSPRITE`), not playback
controls, so `GotoFrame`/`Advance` are not reachable there.

To remove the copyright the next step is advancing the movie's own timeline at boot
(time-scale on the frontend advance, or the AS `gotoAndPlay`/`Invoke` path).

## Deployable: rdmp-launch.exe

`code/launcher` builds `rdmp-launch.exe`, which starts `RDR.exe`, injects
`rdmp-probe.dll`, verifies the module actually loaded (Toolhelp snapshot) and
retries if the injection landed during loader initialisation. It also waits on the
remote thread and prints LoadLibrary's return value, because "inject ok" otherwise
lies. No file in the game folder is created or modified.

Measured with the launcher: copyright at 3 s, **title at ~4.5 s** (baseline ~60 s).
Build outputs: `build2/bin/rdmp-launch.exe` + `rdmp-probe.dll`.

Log path was moved to an absolute workspace file (`_tools/rdmp_skip7.log`) because
`%TEMP%` writes from the launcher-started process silently failed.

### Movie view lookup

Querying the frontend manager (`DAT_142C45610`) for `"startscreen"` from the
injected thread returns `-1` on every attempt, even seconds in, while the frontend's
own calls succeed. Hooking `FUN_14011A000` and capturing the result of the
frontend's own lookup does work: on the game thread, under its lock, the index is
valid (28) and the view object is at `manager+0x30+idx*8`.

Captured view vtable: `0x141CE5318`. Useful slots:

| slot | rva | behaviour |
|---|---|---|
| 08 | `0x1149C0` | `return 0 < *(int *)(view+0x50)` |
| 13 | `0x114A00` | `return *(byte *)(view+0x18)` |
| 19 | `0x114A10` | `return *(int *)(view+0x14)` — **current frame** |
| 29 | `0x1152F0` | `*(int *)(view+0x14) += 1` — **advance frame** |
| 16 | `0x115470` | decrement frame, clamped at 0 |

Writing `view+0x14 = 400` (and re-asserting it from a hook on slot 29 for the first
5 s) moves the title screen from ~4.5 s to ~3.3 s, but the copyright text still
draws during the first ~2.5 s: it is not driven by this playhead in the way the
title is.

Publishing the flash variables on the view object crashes; publishing them on the
definition object (`*state`) is a no-op. So the copyright is not reachable through
`LegalsCompleted` at either level.

The text itself is not findable: `Broadway` / `Epic Games` / `Take-Two` do not appear
in `strings_d11generic.rpf` (scanned all 8,939 payloads, ASCII and UTF-16), and the
two `Rockstar Games, Inc` hits in `RDR.exe` are inside the Authenticode certificate,
not a string table. Conclusion: the screen renders baked glyph outlines (a movie or
a texture), not a localizable string, which is why frame/variable control does not
reach it.

Log: `skip-launcher.log`.




Failed variants, for the record: null Bink alone hangs (no completion); forcing the
`LegalsCompleted` getter returns the engine down a branch that needs a loaded movie
and faults; keys and launch flags do nothing; calling the menu constructor or the
transition off-thread crashes; repeated counter writes re-initialise phases.

### Remaining

- Remove the copyright flash (same controller family; inject earlier or hook the
  text screen's completion).
- Then the frontend work: `FUN_14011A000(manager, "startscreen")` for the Play
  redirect and the Online entry — still runtime-only, via the same DLL.




## Environment

Ghidra 12.2 needs JDK 25. Run headless with `APPDATA`, `LOCALAPPDATA`, `TEMP`
redirected into the workspace (child JVMs are denied writes outside it), e.g.:

```
JAVA_HOME=_tools\jdk25\jdk-25.0.5+8
APPDATA=_tools\ghenv\Roaming
LOCALAPPDATA=_tools\ghenv\Local
TEMP=_tools\ghenv\Temp
```
