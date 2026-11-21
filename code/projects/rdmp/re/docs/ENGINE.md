# Engine findings - RDR.exe 1.0.42.49788435

All addresses are RVAs (image base `0x140000000`). Established by Ghidra
decompilation of the retail binary plus live runs with `rdo-client.dll`.
The server is D3D12; the frontend is Scaleform GFx (ActionScript 2).

## Startup / intro

| RVA | behaviour |
|---|---|
| `0x5F5070` | intro phase controller: publishes `StartOfTransitionOut`, reads `LegalsCompleted` through `0x17F450`, drives `flashText1`/`goText1`/`goLogo` and the Bink logo playback. Called every frame with the state object in `rcx`. |
| `0x5F4390` | per-frame intro wrapper: calls the controller, then `timer = *(int*)(state+8)`; if `timer < 1001` it decrements, otherwise it calls `0x5F4CC0` (the transition). |
| `0x5F4CC0` | leaves the current intro phase (engine state resets; called by the wrapper). |
| `0x5F4690` | constructs the legal phase: loads the `legals` movie through `0x1FCED0`. |
| `0x1FCED0` | builds `"<flash>/legals"` and loads it via `0x11B110`. |
| `0x17F450` | generic AS value truthiness getter (`float*` -> bool). |

State layout (`DAT_142AC5130`): `+0x00` movie, `+0x08` int phase timer, `+0x0d`
transition latch, `+0x11` phase done, `+0x14` post-video countdown, `+0x30`
Bink state. Confirmed by reading it live (`INTRO first tick timer=10000`).

The legal screen is not a timer to shorten: it ends when the `LegalsCompleted`
movie variable becomes true. Publishing it with the engine's own variable API
ends the phase immediately and the wrapper performs the normal transition. The
measured vanilla legal screen is ~22 s; with the skip the title art appears at
~13 s (the remaining time is shader precompilation on this machine).

## Frontend

| RVA | behaviour |
|---|---|
| `0x32FB10` | per-frame title/start-screen state machine. Shows `MainMenuPrompts` and `SS_DLC_Checks`, hides `SinglePlayerPrompts`. On the ACCEPT bit it sets state 2, sends `mainMenuExit` + `enterSinglePlayer`, animates `RDRLOGO.value`/`RDRLOGO.refresh` and shows `waitforInTransition`. **Never called in our offline runs** (vanilla included). |
| `0x335700` | singleplayer start-screen updater: resolves `StartScreen_SinglePlayer`, shows `SS_SP_BackPrompt`, handles the zombie list. Hooked to suppress the picker. |
| `0x2AAC8B8` | frontend controller singleton; `+0xA0` is the UI movie. |
| `0x0EF100` | resolve a named movie clip: `void* (movie, "Name")`. |
| component vtable `+0x160` | show/activate `(component, 1)`. |
| component vtable `+0x158` | hide `(component)`. |
| `0x0FB020` | localization resolve: `const char* (table, mixedKeyHash)`. |
| `0x0C7510` | AS member-name hash; frontend mix `(((h*9)>>11)^(h*9))*0x8001`. |
| `0x181220` | AS variable slot resolver: `uint32* (movie, mixedHash, name)`; `[0]` value, `[4]` type tag (3 = number). |
| `0x11A000` | movie lookup by name (`startscreen` = index 28); views at `manager(0x2C45610)+0x30+idx*8`. |
| `0x2AE3230` | input state pointer; `+0x14` bit `0x20` = ACCEPT. |

Localization keys: `Common_Play` = `0xA11EDE87`, `Common_Settings` =
`0xB7D8B745`, `Common_Quit` = `0xD19CEE9F`.

## UI assets

`game/flash.rpf` is RPF6: header `RPF6`, big-endian TOC at `0x10`, each entry
AES-256 encrypted with 16 iterations, payloads in `RSC\x05` containers with a
zstd frame at `+12`. `tools/rpf6.py` reads it, `tools/rpf_extract.py` extracts it.
The `startscreen` movie is entry `0023_4D8DB2A4` (10.9 MB); its pointers use
Scaleform base-relative tags (`0x50000000` code/data, `0x60000000` textures), so
same-size edits can be patched in place. The logo is the `RDRLOGO` clip; the
title art is vector/bitmap-fill data inside this resource.

## Hooking notes (from building this client)

- The game image and system DLLs (`xinput1_4.dll` at `0x7FFE...`) are more than
  2 GB apart: a single relay page cannot serve both. The client allocates a relay
  page per target region; using the image's page for the XInput hook crashed the
  game instantly.
- Patching must suspend the other threads and check their RIP, otherwise a poll
  loop can resume inside a half-written prologue.
- Prologues of all current targets are position independent within the first
  >=5 bytes, e.g. `0x5F5070`: `push rsi; push rdi; push r15` (9 bytes by full
  decode, 5 is already a boundary), `0x32FB10`: `push rdi; sub rsp,0x40` (6),
  `XInputGetState`: `push rbp; push rbx; push rsi; push rdi` (5).
