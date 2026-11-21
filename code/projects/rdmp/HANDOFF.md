# rdmp handoff

Read this first, then README.md, docs/ARCHITECTURE.md, docs/SCRIPTING.md and CHANGELOG.md (all in this folder).
Reverse-engineering notes: re/NOTES.md and re/docs/.

## The project
Red Dead Multiplayer (rdmp): an MTA-style sandbox multiplayer mod for Red Dead Redemption 1 PC (RDR.exe build
timestamp 0x673783F3), on the MafiaHub Framework 44.1.1. The owner has written permission from MafiaHub (SegfaultD)
to use the Framework freely (this covers AGENTS.md's licence clauses for this project).

Repository: https://github.com/Soapbosnia/Red-Dead-Multiplayer, a fork of the MafiaHub Framework. The Framework
source is untouched at the root; this project lives in `code/projects/rdmp` (MafiaHub's layout for a mod). Commit and
push to `develop`. Build: this project's own `build.bat` (it consumes the pinned Framework SDK; `-DFW_SOURCE_DIR` can
build the Framework from this checkout instead). The root AGENTS.md `builds\build.bat` rule is for Framework targets.

## How we work (the owner's instructions)
* Run everything yourself: build, read `build\build.log`, fix, run the game, read the logs. Don't ask the owner to
  run things you can run.
* No workarounds. Find the real cause (e.g. no injected key presses to hide a bug). If something can't be solved,
  write down what was measured and leave it open.
* Keep paths relative or configurable; nothing hardcoded. Tools belong in `D:\Development\Tools`.
* "Gamemodes" are just resources (scripts).
* Scripting API: MTA names where MTA has the function; clean, structured code; every function documented in
  docs/SCRIPTING.md as SHARED, SERVER or CLIENT. **No element data** (the owner rejected it).
* Implement things properly and in structured modules; update docs + CHANGELOG with each change; atomic commits
  (`Module: description`, wrap at 72), push to `develop`.
* Never write outside the repository (game folder, user profile) - AGENTS.md rule 2.
* Ask before deleting anything outside the repo; prefer the Recycle Bin.

## Machine layout (Windows, owner's PC)
| What | Where |
|---|---|
| repository | `D:\Development\Repositories\Red-Dead-Multiplayer` (project: `code\projects\rdmp`) |
| game | `D:\Games\Red Dead Redemption\RDR.exe` (set in `build\bin\rdmp.ini` `[game] exe`) |
| Framework SDK | `D:\Development\Tools\MafiaHub\Framework-44.1.1-windows` (env `FW_SDK_CACHE_DIR=D:\Development\Tools\MafiaHub`) |
| Ghidra 12.2 | `D:\Development\Tools\ghidra_12.2_PUBLIC` + `jdk-25.0.5+8` (helper `re\gh.ps1`; its headless launcher currently fails to save its JDK setting - use rizin instead) |
| other tools | `D:\Development\Tools\{rizin,x64dbg,Magic-RDR,mingw64,nodejs}` |
| compiler | VS 18 Build Tools (MSVC 14.51, CMake + Ninja); `build.ps1` finds it with vswhere |
| Python | 3.15 on PATH |

`D:\Development\Repositories\rdmp` is the old pre-fork copy of this project: redundant, can be recycled once the owner
agrees.

## Build / run
* `build.bat` (set `RDMP_NO_PAUSE=1` so it does not wait for a key; `build.bat clean`, `build.bat <target>`):
  vswhere -> vcvars64 -> CMake + Ninja Release in `build\`, log `build\build.log`, output `build\bin`.
* Targets: `rdmp` (game DLL), `RDMP` (launcher), `rdmp_client` (Framework client DLL), `RDMPServer`, `rdmp_bot`,
  `rdmp_data` (copies `resources\` and `ui\` to `build\bin` every build; `config\rdmp.ini` only if missing).
* `run.bat` -> `build\bin\RDMP.exe`: starts `RDMPServer.exe` (`[net] start_local_server=1`), starts the game and
  injects `rdmp.dll`. The game opens on the main menu; join the local server from its Local tab.
  `[net] autoconnect=1` connects straight to `[net] server` instead.
* Second player: `build\bin\rdmp_bot.exe bot Bob 127.0.0.1 27015 [x y z radius seconds]`.
* Logs: `build\bin\rdmp.log` (game + client, everything), `build\bin\logs\` (Framework files, cef.log).
* Server console: `players`, `tp`, `help`, and the Framework's `start|stop|restart|refresh <resource>`, `status`.

## Status (0.6.0, tested in game)
Works: main menu (HTML, server browser Internet/Local/Favorites/Recent, settings, join/leave/rejoin), D3D12
overlay (Framework renderer + ImGui + CEF), chat box (T), nametags, dx* drawing (scoreboard on Tab), blips (own blip
hidden), death with animation (killPed) and respawn without a crash, resource stop/restart removing its elements,
rdmp_bot as a second player, server `/rdmp` info endpoint.

## Architecture (short; docs/ARCHITECTURE.md has more)
* `rdmp.dll` (src/game, static CRT, no Framework): boot/sandbox (sp_idle root script, menus skipped, player in limbo
  until a script spawns them), game natives (`scriptnatives`, with a small table for natives whose retail
  registration is skipped), `host.cpp` = the `fw::Host` table for the client DLL, `netclient.cpp` = loads
  `rdmp_client.dll` and pumps it once per frame on the game's main thread (actions, puppets for remote players,
  blips).
* `src/shared/fw_api.h` (v6): the C ABI between the two DLLs. Bump the version on any change; rebuild both.
* `rdmp_client.dll` (src/client): `fw_client.h` (Framework client instance), `overlay.cpp` (D3D12 Present /
  ExecuteCommandLists hooks, DirectInput hooks so typing never reaches the game; borrows the game's swapchain and
  queue, never AddRefs them - the game checks), `hud.cpp` (dx* queue, nametags, chat box), `frontend.cpp` +
  `server_query.cpp` + `ui/` (main menu), `api_*.cpp` (client script API).
* `RDMPServer` (src/server): `main.cpp` (instance, connections, chat -> playerChat/playerCommand, /rdmp endpoint),
  `state.cpp` (action channel, ownership, deaths, chat), `api_*.cpp` (server script API).
* `src/shared/script_api.h`: helpers + the SHARED functions; `player.h` / `blip.h`: replicated entities.

## Key RE facts (build 0x673783F3, base 0x140000000)
* Native registry: runtime table `*(0x142B0D210)` of {u32 hash, u64 handler}, capacity `*(u32*)0x142B0D218`;
  slot = hash % cap, step (hash>>1)+1. Full list with handlers: `re/natives_0x673783F3.csv`.
* NativeContext: +0 result*, +8 argc, +0x10 args* (8-byte slots), +0x18 vecCount, +0x20 vecRefs[4], +0x40 vecData.
  Results can be several slots (Vector3): NativeCall keeps a 64-byte buffer.
* Actor handles: scripts take GET_PLAYER_ACTOR(0) (e.g. 529), not playerInfo+0x5ec (e.g. 2).
* Camera: `*(*(0x142C45738)+0x28)`: +0x00 half vertical FOV (deg), +0x10 right, +0x20 up, +0x30 back, +0x40 position.
  GET_CAMERA_POSITION crashes with the GET_GAME_CAMERA handle - read memory instead.
* Text: PRINT_OBJECTIVE_B draws the bottom line (one at a time); PRINT_BIG the big title; PRINT_SMALL_B and
  PRINT_HELP_B draw nothing in the sandbox. DRAW_STRING_CURRENT_FONT is a debug native whose queue draws nothing.
* Input: the game reads keyboard and mouse through DirectInput 8 (GetDeviceState / GetDeviceData).
* Native menus: Scaleform in flash.rpf; adding pages needs new SWF content (re/docs/FRONTEND_MAP.md) - not done, the
  HTML menu replaces it; game settings stay in the game's pause menu.

## Open
1. **Respawn freeze.** After a death + respawn the screen keeps showing the old body until the game reads any keyboard
   input through DirectInput (even an unbound key such as F12 ends it). Measured: the game camera
   (*(camMgr+0x28)) is already behind the new player, the camera manager's first 0x2000 bytes do not change when the
   key ends it, game time keeps running, the game is not in pause state 4, and CLEAR_DEATHFX /
   END_CURRENT_CAMERA_SHOT_TRANSITION / RESET_TIME_SINCE_LAST_INPUT do not end it. Next: find what consumes "any key"
   after a death (follow the DirectInput keyboard state into the game, or diff render / game state objects before and
   after the key). No workaround (no injected input).
2. LAN discovery for the Local tab (today: this PC, ports 27015-27020). Masterlist announcing needs a server token.

## Next (chosen: horses first)
1. **Horses and horse sync**: spawn a horse from a script, mount / dismount, sync rider + horse movement so others see
   it, MTA-style vehicle functions (createVehicle-like, warpPedIntoVehicle, getPedOccupiedVehicle, ...).
2. Weapons and damage sync. 3. Peds (createPed; killPed already accepts peds by design). 4. Markers / colshapes.
5. Client-side blips, addCommandHandler. 6. World control (weather, time). 7. Persistent accounts.
