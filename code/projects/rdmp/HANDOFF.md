# rdmp handoff (2026-11-21)

State of the work when it moved from a claude.ai chat (no shell on the PC) to a Claude Code session. Read this first,
then README.md, docs/ARCHITECTURE.md, docs/SCRIPTING.md and CHANGELOG.md.

## The project
Red Dead Multiplayer (rdmp): an MTA-style sandbox multiplayer mod for Red Dead Redemption 1 PC (RDR.exe build
timestamp 0x673783F3), on the MafiaHub Framework 44.1.1 SDK. The owner has written permission from MafiaHub
(SegfaultD) to use the Framework freely.

## Machine layout (Windows, user's PC)
| What | Where |
|---|---|
| this repo | `D:\Development\Repositories\rdmp` |
| game | `D:\Games\Red Dead Redemption\RDR.exe` |
| Framework SDK | `D:\Development\Tools\MafiaHub\Framework-44.1.1-windows` (stamp `.fw-extracted` = pin SHA-256) |
| vcpkg checkout | `D:\Development\Tools\MafiaHub\vcpkg` |
| env var | user `FW_SDK_CACHE_DIR=D:\Development\Tools\MafiaHub` |
| Ghidra 12.2 | `D:\Development\Tools\ghidra_12.2_PUBLIC` (+ `jdk-25.0.5+8`), project `D:\Development\Tools\ghidra-projects\rdr\*.gpr` (helper: `re\gh.ps1`) |
| other tools | `D:\Development\Tools\{x64dbg,rizin,Magic-RDR,mingw64,nodejs}`, `Tools\reference\unethical-gangsters` |
| compiler | VS 18 Build Tools (MSVC 14.51, CMake + Ninja bundled); `build.ps1` finds it with vswhere |
| Python | 3.15 on PATH |

Old trees still on disk, to be deleted ONLY after the new build is tested in game and the owner confirms the list:
`D:\Development\Repositories\rdo`, `D:\Development\Repositories\rdo.pre-rdrmp` (useful parts already salvaged into
`re/`), `D:\Development\Repositories\_incoming` (SDK zip, upstream zip, cltest), `D:\fwb` (old build output + old
resources), leftover zips (ghidra.zip, jdk21/25 zips) and old jdk21 / framework 40.2.0 / cmake-4.4.5 folders. Prefer
the Recycle Bin.

## Build / run
* `build.bat` (or `powershell -File build.ps1 [clean] [targets]`): vswhere → vcvars64 → `cmake -G Ninja` Release in
  `build\`, `ninja -k 0`, full log in `build\build.log`. Outputs in `build\bin`.
* Targets: `rdmp` (game DLL, static CRT, no Framework), `RDMP` (launcher), `rdmp_client` (Framework::Client DLL),
  `RDMPServer` (Framework::Server), `rdmp_bot` (headless test client), `rdmp_data` (copies `resources\` →
  `build\bin\resources` every build and `config\rdmp.ini` → `build\bin\rdmp.ini` only if missing).
* `run.bat` → `build\bin\RDMP.exe`: starts `RDMPServer.exe` if `[net] start_local_server=1` and server=127.0.0.1,
  asks once for RDR.exe (saved to `[game] exe`), starts the game suspended and injects `rdmp.dll`.
* Logs: `build\bin\rdmp.log` (game DLL), `build\bin\logs\` (Framework client/server).

**Status (2026-11-21, 0.6.0): BUILD OK, tested in game.** Repository: github.com/Soapbosnia/Red-Dead-Multiplayer
(the Framework fork; this project is `code/projects/rdmp`, history kept). Works: main menu (server browser, settings,
join / leave / rejoin), D3D12 overlay, chat, nametags, scoreboard, blips, death and respawn, resource restarts, bot.
Open: the respawn freeze below. Native menus were not modified: see re/docs/FRONTEND_MAP.md for why (new pages need
new Scaleform content in flash.rpf); the HTML menu replaces that plan, game settings stay native (pause menu).

## Architecture (short)
* `rdmp.dll` (src/game): boot/sandbox (sp_idle root script, menus/legals skipped, player created, MTA-style limbo
  until a script spawns them), native registry access (`scriptnatives`), `host.cpp` = the `fw::Host` table given to
  the client DLL, `netclient.cpp` = loads `rdmp_client.dll` from rdmp.ini's folder (`[net] fw_dir` overrides) and
  calls `FwStep()` once per frame on the game's main thread: set_local, update, actions, forced poses, remotes
  (puppets), link, blips (`blips.cpp` reconciles with the game blip pool).
* `src/shared/fw_api.h` v5: C ABI between the two DLLs (`Host`, `Api`, export `rdmp_client_get_api`). Bump the
  version on any change; both DLLs must be rebuilt together.
* `rdmp_client.dll` (src/client): `FwClient` (Framework client instance, headless; creates an ImGui context first —
  this is the fix for the crash on death: the Framework chat box called `ImGui::GetTime()` with no context when the
  wasted script's chat line arrived). `client_natives.cpp` registers client script globals via ModuleRegister and
  emits `clientKey` / `clientRender` with `Events::EmitReservedSync` (the async variant never ran the handlers). Chat
  lines are printed in game with PRINT_OBJECTIVE_B (host.cpp HostPrint).
* `RDMPServer` (src/server/main.cpp): Player + Blip entities, acked action queue (spawn, fade, camera, control,
  health), server script API (see docs/SCRIPTING.md), elements tracked per creating resource and destroyed in
  `AddOnResourceStopped`, blip visibility via viewer filters, console `players` / `tp` / `help`, env
  `RDMP_AUTOKILL`, `RDMP_AUTO_RESPAWN`.

## Key RE facts (build 0x673783F3, base 0x140000000, no ASLR shift)
* Native registry: `lea r8,[handler]; mov edx,joaat(lowercase name); call 0x1401E7140`. Runtime table
  `*(0x142B0D210)` of 16-byte {u32 hash, u64 handler}, capacity `*(u32*)0x142B0D218`, count `0x142B0D220`;
  slot = hash % cap, step (hash>>1)+1. Self-check anchor VDIST 0x3C08ECB7 → 0x1401E6EF0. Full list:
  `re/natives_0x673783F3.csv`.
* NativeContext: +0 result*, +8 argc, +0x10 args* (8-byte slots), +0x18 vecCount, +0x20 vecRefs[4], +0x40 vecData.
* Blips: ADD_BLIP_FOR_COORD 0xC6F43D0E (Vector3*, icon, minDist, bool, int); pool 199 slots at loader+0x2270;
  icons 299..572 (BLIP_OBJECTIVE 330, BLIP_FRIEND 325, BLIP_PLAYER 333, BLIP_MP_COLOR_1 356..); SET_BLIP_COLOR
  takes floats 0..1.
* Text: PRINT_OBJECTIVE_B (text, float s, bool literal, int, bool, ptr, bool, ptr) draws the bottom-centre line;
  a new one is dropped while one shows, CLEAR_PRINTED_OBJECTIVE first then print ~400 ms later. PRINT_BIG draws big
  centre text. PRINT_SMALL_B is never accepted in the sandbox; PRINT_HELP_B is accepted but not drawn.
* Actor handles: scripts take GET_PLAYER_ACTOR(0) (e.g. 529), not playerInfo+0x5ec (e.g. 2). SET_ACTOR_HEALTH 0xFA090024 (actor, float).
* More in `re/NOTES.md` and `re/docs/`.

## Resources (resources/)
* `spawner` (server): on playerConnected → fadeCamera out, spawnPlayer at Armadillo station
  (-2208.91, 16.51, 2605.41, heading -100.3), setCameraTarget, fade in after 500 ms; per-player tokens.
* `wasted` (server): on playerWasted → chat line, fade out, respawn after 4 s, fade in.
* `playerblips` (server): createBlipAttachedTo(player, BLIP_MP_COLOR_1, 2, random rgb, 255, 1, 16383) per player.
  `SHOW_OWN_BLIP = true` for now (owner tests alone); set false to hide each player's own blip from them.
* `devtools` (client): F7 position, F8 die (`invokeNative("KILL_ACTOR", "i>n", getLocalActor())`),
  F9 player list.

## To do
1. **Respawn freeze.** After a death + respawn the screen keeps showing the old body until the game reads any keyboard
   input through DirectInput (even an unbound key such as F12 ends it). Measured: the game camera
   (*(camMgr+0x28)) is already behind the new player, the camera manager's first 0x2000 bytes do not change when the
   key ends it, game time keeps running, the game is not in pause state 4, and CLEAR_DEATHFX /
   END_CURRENT_CAMERA_SHOT_TRANSITION / RESET_TIME_SINCE_LAST_INPUT do not end it. Next: find what consumes "any key"
   after a death (follow the DirectInput keyboard state into the game, or diff the render / game state objects before and
   after the key). No workaround (no injected input).
2. Next features: see the proposals in the last session summary (vehicles/horses, peds, markers/colshapes, timers via
   the Framework, client-side blips, server browser / connect screen, CEF web UI through the Framework's Web builtin).
