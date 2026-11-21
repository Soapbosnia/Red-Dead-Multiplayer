# RE Plan — from zero to "two players see each other"

Target: `RDR.exe` **1.0.42.49788435**, SHA-256 `4D3F7A26…`, at
`D:\Games\Red Dead Redemption\`. Facts behind this plan are in
[FINDINGS.md](FINDINGS.md). Framework background is in
[the Framework's new-project guide](https://github.com/MafiaHub/Framework/blob/develop/docs/starting_a_new_project.md).

## Ground rules

1. **No invented addresses.** Every RVA, offset and signature in this repo is
   either derived from the binary or not written down at all. Guesses cost more
   than gaps.
2. **Patterns are build-scoped.** A signature is valid for one exe hash. The
   build id is logged at startup and mismatches fail loudly.
3. **Read-only static analysis is safe; dynamic analysis is not.** Back up saves,
   keep a pristine copy of `RDR.exe`, and never let a tool write into the game
   folder except an explicitly documented mod DLL.
4. **Milestones are gates.** Milestone *n+1* does not start until *n* passes on a
   cold game start, not just once.

## 0. Prerequisites (blocking)

| Item | Status |
|---|---|
| MSVC 2022 (v143) + Windows SDK | **Missing** — only MinGW + Node are installed |
| CMake ≥ 3.20 | Check after MSVC install |
| MafiaHub Framework SDK (release zip or source checkout) | Not downloaded |
| `FW_RPC_IDENTIFIER_SALT` + services licence (MafiaHub Discord) | Not requested |
| x64dbg + ScyllaHide | Not installed |
| Cheat Engine | Not installed |
| IDA Free 8.x or Ghidra | Not installed |
| Python | Bundled DSH runtime — works (`tools/pe_info.py` already runs) |

MinGW cannot substitute for MSVC here; see [BUILD.md](BUILD.md) for the ABI
reasons. This is the first thing to fix.

## 1. Milestones

| # | Milestone | Gate (done when) | Work type |
|---|---|---|---|
| M1 | Injection | Client DLL loads into `RDR.exe` and appends a version line to `rdmp.log` on a cold start | RE + launcher |
| M2 | The tick | A per-frame counter matches wall-clock frames for 60 s, including after a loading screen | RE |
| M3 | Patterns | Five signatures resolve uniquely against this exact exe hash | RE |
| M4 | Shell | `Application::Init` succeeds; an ImGui window draws over the D3D12 frame | Framework |
| M5 | **Player replication** | Two clients connect and see each other move, die and respawn | Framework |
| M6 | Hardening | Reconnect is clean; a TS resource can move a player | Framework |

Estimates, not promises: M1–M3 are the long pole (weeks, RE-dependent). M4 is
days. M5 is one to three weeks once the layout is right.

## 2. M1 — injection

**Recommended: the Framework launcher.** `Framework::Loader` maps the game into its
own process (`ProjectLaunchType::PE_LOADING`) and brings the client DLL up with it —
the path M2O and M3O use. It avoids the cross-process `DLL_INJECTION` build variant,
which exists mainly because injectors get flagged by antivirus.

**Fallback: proxy DLL.** `RDR.exe` imports `DINPUT8.dll` directly, so a
`dinput8.dll` proxy (with forwarded exports) is technically available. Before
committing to it, answer the integrity question: `v1.0.42.blake3` lists shipped
files by hash. Adding a *new* DLL does not change any listed hash; replacing
`dinput8.dll` does. Test which one the game tolerates.

**Sequence.**

1. Build a do-nothing client DLL that opens `rdmp.log` and writes the framework
   version, exe hash and timestamp. No hooks.
2. Launch through the Framework loader; confirm the log appears.
3. Reboot, cold-start, confirm again.
4. Only then start hooking.

**Failure modes to expect:** the launcher process being denied access to the game
window; the Streamline interposer rejecting an unknown overlay; the game's own
integrity check aborting on a modified directory.

## 3. M2 — the tick

Every mod needs one call site per frame and one at shutdown. We need a *cadence* to
drive replication even before we know the game's own update function, so this is
two separate findings:

**A. Frame cadence (easy, reliable).**
Hook `IDXGISwapChain::Present` (or `ID3D12CommandQueue::ExecuteCommandLists`) via
the `dxgi` vtable. The Framework's D3D12 backend already hooks presentation for its
overlay — reuse that signal rather than adding a second present hook. It gives a
monotonic frame counter and a safe place to run UI code. It is **not** a game tick:
it does not tell us when the simulation advanced.

**B. The simulation tick (the real work).**

1. Break on `Present`, then walk the call stack. RAGE's main loop typically calls a
   frame-dispatch function that updates time, streaming, AI, physics and scripts in
   a fixed order.
2. Independently, find the game clock: Cheat Engine, scan for the float seconds
   value, find what writes it. The writing function is the timer update; its caller
   is the frame dispatcher. Two independent routes agreeing on one function is the
   confidence we need.
3. Verify with the RDR2 lineage: `d11-engine` class names and call shapes should
   look familiar to anyone who has read RDR2 mod sources. Treat every RDR2 address
   as a hypothesis; only the *shape* transfers, never the offset.

**Gate:** a counter incremented from the candidate tick matches the present-hook
frame count 1:1 for 60 s of gameplay, survives a loading screen (it should pause or
skip, not drift), and runs on the same thread every frame.

## 4. M3 — the first five patterns

A pattern is a byte signature with wildcards over operands:

```
48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? ?? ??  (illustrative shape only)
```

**Workflow.** Pick a function identified in M2 → take 24–32 bytes from its prologue
→ wildcard every `rel32`/`imm32`/`disp32` → verify the signature matches **exactly
once** across `.text` raw bytes → store it in `rdmp.patterns` with a comment naming
the function and the exe hash it was derived from. A signature that matches twice
is not a signature; it is a coin flip.

| # | Target | How to locate it | Why it matters |
|---|---|---|---|
| 1 | Frame tick | M2-B | Drives everything |
| 2 | Local player ped | Walk from the camera or the HUD: find the object whose position equals the camera's focus; or start from a coordinate in memory and pointer-scan upward to a static base | The thing we replicate |
| 3 | Ped transform (position/heading/velocity offsets) | Hardware breakpoint on the coordinate floats; watch which offsets the game reads/writes each frame | Wire format |
| 4 | Ped creation / clone | Spawn an NPC, break on allocation, walk back to the factory | Spawning remotes |
| 5 | Model / streaming request | Request a known model and trace the call | Making remotes visible |

Targets 3–5 are struct offsets, not signatures: document them as
`field @ offset` with the evidence (breakpoint address, instruction) that produced
them, in code comments next to the struct.

**Tooling gap to close:** a uniqueness checker (`tools/sigcheck.py`) that takes a
pattern, counts matches in `.text`, and prints candidate addresses. Small, and it
prevents the most common self-inflicted wound in this phase.

## 5. M4 — the shell

Follow the Framework guide exactly: `Application` (framework lifecycle) and
`ApplicationModule` (game tick adapter) stay separate; boot hooks live in
`core/boot/`. Select the **D3D12** renderer backend — it exists
(`graphics/backend/d3d12.cpp`, `d3d12_ui_compositor.cpp`). Gate: ImGui window draws,
game still renders, streamer still works.

## 6. M5 — player replication

The shared contract is the whole design; get it right once:

- `SerializeConstruction` — spawn metadata (nickname, model, flags).
- `SerializeTransform` — position/velocity/heading on the unreliable channel.
- `SerializeFields` — reliable deltas (health, weapon, vehicle seat).
- Server-owned fields via `f.ServerField(...)` for anything the server decides.

Client `Player` subclasses the shared entity: the owner captures native state
upstream, remotes apply what arrives. The server creates one entity per connection
and registers it as that connection's viewer (the avatar *is* the interest-grid
origin).

**Gate:** two clients, both moving, both seeing smooth motion; a death and respawn
propagates; a disconnect removes the remote ped.

## 7. Method notes

**Static.** `tools/pe_info.py` for fingerprinting; IDA Free or Ghidra for
decompilation. Remember `.data` is mostly zero-fill (5.5 MB raw behind 1.48 GiB
virtual) — bound scans to raw ranges. The RDR2 engine lineage means RDR2 mod
repositories are the best available reference for class *shapes*.

**Dynamic.** x64dbg + ScyllaHide for breakpoints and stack walking; Cheat Engine for
value discovery and pointer scans. Dynamic steps need a human at the keyboard —
I can write x64dbg scripts and analyse whatever you paste back, but I cannot drive
the debugger from here. Budget for a shared session or async screenshot exchanges.

**Ordering discipline.** Find data first, then code that touches it, then the class
that owns it. Naming comes last, from observed behaviour, not from a guessed vtable.

## 8. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Anti-tamper/integrity check rejects the mod | Blocks everything | Test M1 before any RE investment; keep the install pristine; prefer the loader over file replacement |
| No gameplay RTTI → slow class discovery | Weeks of extra work | Lean on RDR2 corpus for shapes; accept pointer-scan workflows |
| Patch drift (1.0.42 → newer) | Patterns break | Version patterns by exe hash; fail loudly with a clear message |
| D3D12 + Streamline/DLSS overlay conflicts | UI broken or crash | Test with DLSS on/off; hook after the interposer |
| Replication bandwidth with many peds/vehicles | Rubber-banding | Interest grid is built in; transform on unreliable channel; measure early |
| Framework services need a licence | No masterlist/accounts | Request early from MafiaHub Discord; a LAN/direct-connect path works without them |
| Legal/ToS exposure | Reputation, takedowns | Single-player game, own netcode, no Rockstar services, no game-file redistribution |

## 9. First two weeks

- [ ] Install VS 2022 Build Tools + Windows SDK (see [BUILD.md](BUILD.md)).
- [ ] Clone/download the Framework SDK and build the smoke target.
- [ ] Join MafiaHub Discord; request SDK services access and the RPC salt.
- [ ] Install x64dbg + ScyllaHide and Cheat Engine.
- [ ] M1: do-nothing DLL + log line through the loader.
- [ ] M2: present-hook cadence; then the game clock hunt.
- [ ] Write `tools/sigcheck.py`; derive signature #1 (the tick).
- [ ] Record every finding in `docs/FINDINGS.md` with its evidence.
