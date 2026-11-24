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
* `run.bat` -> `build\bin\RDMP.exe`: starts the game and injects `rdmp.dll`; the game opens on the main menu.
  Development: `RDMP.exe --server --connect 127.0.0.1:27015` also starts `RDMPServer.exe` and joins it at once
  (`[net] start_local_server` / `autoconnect` do the same from the ini); `tools\run_two.ps1` starts the server and
  two copies joined to it.
* Second player: `build\bin\rdmp_bot.exe bot Bob 127.0.0.1 27015 [x y z radius seconds]`.
* Logs: `build\bin\rdmp.log` (game + client, everything), `build\bin\logs\` (Framework files, cef.log).
* Server console: `players`, `tp`, `help`, and the Framework's `start|stop|restart|refresh <resource>`, `status`.

## Status (0.6.0, tested in game)
Works: main menu (ImGui, see "Main menu" below: server browser Internet/Local/Favorites/Recent, settings,
join/leave/rejoin, Esc in-game menu), D3D12 overlay (Framework renderer + ImGui), chat box (T), nametags, dx* drawing (scoreboard on Tab), blips (own blip
hidden), death with animation (killPed) and respawn without a crash, resource stop/restart removing its elements,
rdmp_bot as a second player, server `/rdmp` info endpoint.

## Architecture (short; docs/ARCHITECTURE.md has more)
* `rdmp.dll` (src/game, static CRT, no Framework): boot/sandbox (sp_idle root script, menus skipped, player in limbo
  until a script spawns them), game natives (`scriptnatives`, with a small table for natives whose retail
  registration is skipped), `host.cpp` = the `fw::Host` table for the client DLL, `netclient.cpp` = loads
  `rdmp_client.dll` and pumps it once per frame on the game's main thread (actions, puppets for remote players,
  blips).
* `src/shared/fw_api.h` (v12): the C ABI between the two DLLs. Bump the version on any change; rebuild both.
* `rdmp_client.dll` (src/client): `fw_client.h` (Framework client instance), `overlay.cpp` (D3D12 Present /
  ExecuteCommandLists hooks, DirectInput hooks so typing never reaches the game; borrows the game's swapchain and
  queue, never AddRefs them - the game checks), `hud.cpp` (dx* queue, nametags, chat box), `frontend.cpp` (menu
  state + data, `menu_model.h`) + `menu_ui.cpp` (ImGui main menu) + `server_query.cpp` (main menu), `api_*.cpp` (client script API).
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
* Native menus: SCXML data driving Scaleform movies (re/NOTES.md "Native menus"); explored, then retired for the
  ImGui menu (title_screen=1 still runs them).

## Open
1. **Respawn freeze.** After a death + respawn the screen keeps showing the old body until the game reads any keyboard
   input through DirectInput (even an unbound key such as F12 ends it). Measured: the game camera
   (*(camMgr+0x28)) is already behind the new player, the camera manager's first 0x2000 bytes do not change when the
   key ends it, game time keeps running, the game is not in pause state 4, and CLEAR_DEATHFX /
   END_CURRENT_CAMERA_SHOT_TRANSITION / RESET_TIME_SINCE_LAST_INPUT do not end it. Next: find what consumes "any key"
   after a death (follow the DirectInput keyboard state into the game, or diff render / game state objects before and
   after the key). No workaround (no injected input).
2. LAN discovery for the Local tab (today: this PC, ports 27015-27020). Masterlist announcing needs a server token.
3. Done: the game boots to the main menu and starts no server; auto-join / local server only with `--connect` /
   `--server` (or the ini keys) for development.

## Main menu (2026-11-23)
Owner's call (2026-11-22): the native menus took too long, ImGui instead, themed, NO CEF main menu. Done and pushed
(179ad9de), checked in game: src/client/menu_ui.cpp draws it, frontend.cpp keeps its data (menu_model.h).
* MTA flow ([game] title_screen=0, [client] menu=1, the defaults): the world loads at boot with the player in limbo,
  the full-screen menu covers it while not on a server; join = connect only, Disconnect = leave + limbo + menu (no
  world reload; rejoin checked).
* MTA-style (owner, 2026-11-23): a title screen (type logo, QUICK CONNECT / BROWSE SERVERS / SETTINGS / ABOUT /
  QUIT, "Latest news" from ui/changelog.txt - keep it player-facing, version) and movable ImGui windows over it:
  Server Browser (tabs, search, filters, sortable table, double-click / Join, Favourite, password dialog), Quick
  Connect, Settings (Multiplayer + the game's own options; Apply / Revert, display changes revert in 15 s unless
  kept), About. Esc closes the last window opened.
* Type: Segoe UI in four weights (owner: a clean bold sans, not the game's bitmap fonts; Roboto is not on Windows).
  Art read at runtime from the player's game files (src/client/rdr_assets.cpp; never shipped; re/NOTES.md "Game fonts
  and title art"): the logo is the title movie's brush letters RED / DEAD in the original red, MULTIPLAYER on a dark
  brush stroke; menu items get a red brush stroke on hover. Background: drifting glows and embers (menu_ui.cpp).
* Chat box and nametags are the mod's own (hud.cpp): the Framework's ChatBox only keeps the open / visible / session
  state (T and the scripts' Chat.open drive it); lines come from OnChatMessageReceived and Hud::ChatLine, typed lines
  raise "chatSend" then SendChatMessage, as the Framework's box does.
* Settings > Binds (MTA-style): chat key, resource binds (client `bindKey` with a description; overrides in rdmp.ini
  [resource_binds]), the game's controls by category, primary / secondary (live through the game's own commit, kept in
  rdmp.ini [game_binds], reapplied at launch; gabindings.xml untouched). re/NOTES.md "Key bindings".
* The game's own saves fail here (gabindings.xml and options.xml unchanged since 2026-11-03; why is open), so game
  settings changed in Settings are kept in rdmp.ini [game_options] (the ones that differ from the game's) and applied
  at launch like the binds. Verified: Radar off survives a relaunch, turning it back on removes the line.
* In a session Esc opens the in-game menu over the darkened world: Resume, Settings, Disconnect (no prompt), Quit.
  While the menu is up: cursor shown, the game gets no keyboard / mouse (DirectInput + window hooks) and
  set_input_lock holds the player (checked: W does not move him). Esc never reaches the game (WM_KEYDOWN, raw input,
  DirectInput state and buffered events), so the game's pause menu never opens.
* Fonts: Windows' bahnschrift.ttf (UI), palab / pala.ttf (headings, logo), segmdl2.ttf (icons), through the
  Framework's imguiFontPath / imguiFonts (fw_client.h). Sizes are written for 1080 p and scaled with the window.
* NOT checked yet: a real Esc key press (automation cannot press Esc; WM_KEYDOWN posted to the window was used), so
  the DirectInput Esc filter (the game's own pause menu staying shut) needs the owner's check; a server drop while
  joined (should show "Disconnected from the server." on the menu); the keep / revert display prompt.
* Ideas: a texture logo (ui/assets/logo.svg rasterised) instead of the type-only one; gamepad navigation
  (ImGuiConfigFlags_NavEnableGamepad); LAN discovery for Local.
* The native-menu work (uifiles.cpp, ui/native/, disconnect_unload, ui_exec debug commands) stays behind
  title_screen=1 for reference; re/NOTES.md "Native menus" and "Disconnect" hold what was learned.

Test automation: keys through computer-use only when RDR.exe is in front, NEVER Esc (it stops Claude): post
WM_KEYDOWN / WM_KEYUP VK_ESCAPE to the game window instead (PowerShell PostMessage). Start RDMP.exe without the shell
(Process.Start with UseShellExecute=false): Start-Process triggers Windows' "Open File - Security Warning". The
Claude app window may sit over part of the screen and swallow clicks there (check with WindowFromPoint).
Start RDMPServer.exe for the Local tab / Direct Connect 127.0.0.1:27015 (stop the game before building).

## Weapons and damage (in progress, 2026-11-23; owner's plan: weapons + damage, then element functions)
* Done: script API (docs/SCRIPTING.md "Weapons"), weapon in hand + ammo synced (Player::weapon / ammo, owner
  fields), puppets armed and invulnerable locally; aim (input 0x19) and fire (0x09 / 0x0d) are among the 30 synced
  controls. Sidearms and long guns give / draw (long guns needed their equip slots enabled). re/NOTES.md "Weapons".
* Next: (1) done: long guns; (2) aim (re/NOTES.md "Aiming", "Weapon state sync"): the weapon state is synced (draw, ready, aim, fire, reload,
  holster: the puppet runs the game's own weapon state machine from the remote's real state); facing and aim point too;
  puppets have private input mappers (no crosstalk from the local weapon wheel). OPEN: arm pitch (re/NOTES.md "arm
  pitch": the aim task has the right target, the animation only pitches on its player branch)
  (MTA-style: the remote's camera in the puppet's context, their body heading while aiming). OPEN: the arm's pitch (the
  puppet aims level whatever the remote's pitch; the aim matrices are right now, so the pitch comes from somewhere else,
  probably the aim parameters the camera writes for the local player only, FUN_14037da20); fire at a remote's target; (3) damage: local health drops -> GET_LAST_ATTACKER / GET_LAST_HIT_WEAPON /
  GET_LAST_HIT_ZONE (now resolvable, scriptnatives.cpp) -> client `clientPlayerDamage` (cancellable: restore health;
  lethal hits need a hook on the damage function to cancel before death) and server `playerDamage`, killer on
  `playerWasted`; (4) the weapon wheel's slow motion (not the timestep at 0x142273750, not playerInfo+0x3d4; a
  flag at playerInfo+0x104 bit 0x20000 is set while the wheel is open); (5) Tab is both the weapon wheel and the
  scoreboard's default bind.
* Test aids: [debug] aim_hold=2 makes game copy 2 hold its aim input (its player really aims; aim_hold_point=1 sends a
  test point instead: aim_hold_yaw / aim_hold_sweep / aim_hold_pitch / aim_hold_pitch_swing); aim_trace=1 logs the puppet's
  heading against the remote's every 100 ms; state_probe=1 logs the local player's and every puppet's weapon state on
  each change (the two copies' logs line up); weapon_probe=1 also binds aim to K and fire to L; fire_probe=<input id>
  watches who reads that input record; exec_watch=0x14... logs the first calls of a game function. Two copies:
  start them from bash (`./RDMP.exe --server --connect 127.0.0.1:27015`, then `--instance 2 --connect ...`): this PC's
  PowerShell policy refuses unsigned scripts, and Start-Process (ShellExecute) stops on Windows' unsigned-publisher
  prompt. computer-use mouse moves reach the game only right after a click in it (the game re-centres the cursor). [debug] weapon_probe=1 logs weapon names, input changes and the weapon in hand, and binds aim to K too
  (automation cannot hold a mouse button and a key together).

## Next (chosen: horses first)
1. **Horses: done (first version)** - createVehicle & co., E mount / dismount synced, rider syncs the horse. Left:
   remote horses are driven by the rider's movement request (desire mode), not yet by the rider's horse controls
   through the context switch (playerInfo+0x138 input_horse, MarstonBehave state 5); horse health / death, carriages.
2. Weapons and damage sync. 3. Peds (createPed; killPed already accepts peds by design). 4. Markers / colshapes.
5. Client-side blips, addCommandHandler. 6. World control (weather, time). 7. Persistent accounts.
