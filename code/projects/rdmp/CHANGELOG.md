# Changelog

## Unreleased: remote-player sync rework
* **Game settings survive a restart:** the game's own save fails here (options.xml unchanged since 2026-11-03), so the
  settings changed in Settings (display, graphics, audio, controls, interface) are kept in rdmp.ini [game_options] (only
  the ones that differ from the game's) and applied over the game's at every launch, like the binds.
* Fixed: percentage sliders showed "100" instead of "100%".
* **Settings > Binds** (MTA-style): multiplayer controls (the chat key), resource binds (every bind a server script made
  with a description, per resource) and the game's own controls by category with primary / secondary keys; click a key,
  press the new one (Backspace / Delete unbinds, Esc cancels), * marks changes, Reset game controls. Game controls go
  live through the game's own commit (its keybind menu's path) and are kept in rdmp.ini [game_binds], applied over the
  game's at every launch: the single-player gabindings.xml is never changed. fw_api v14 (quit, get_bindings,
  set_binding).
* **Client scripting: `bindKey(key, state, handler [, description])` / `unbindKey`** (scripting layer: MINOR). The
  scoreboard resource binds Tab with it, so players can move it.
* Fixed: Quit only made the game beep (a WM_CLOSE to its window); it now ends the game like its own Quit.
* The title screen no longer shows the news panel or "Playing as".
* **The game's own fonts and logo art in the menu**, read at runtime from the player's game files (nothing extracted is
  shipped): the slab serif, small serif and condensed faces from fonts.rpf (served to ImGui through a custom font
  loader, so every widget, the chat and nametags use them) and the title movie's brush letters for a RED DEAD
  MULTIPLAYER logo; menu items get the game's red brush stroke on hover. fw_api v13 (Host inflate: the game's own
  decompressor).
* **Main menu, MTA-style:** a title screen (logo, QUICK CONNECT / BROWSE SERVERS / SETTINGS / ABOUT / QUIT, latest
  news, version) with the server browser, quick connect, settings and about as movable ImGui windows over it; Esc
  closes the last window opened. In game: RESUME / SETTINGS / DISCONNECT / QUIT over the darkened world.
* **ImGui main menu** (src/client/menu_ui.cpp, frontend.cpp): replaces the CEF HTML menu (ui/index.html, app.js,
  style.css removed) and the native SCXML menus as the default. Home (recent servers, changelog from
  ui/changelog.txt), server browser (Internet / Local / Favorites / Recent, search, filters, sorting), direct
  connect, settings (multiplayer + the game's own). MTA flow: the world loads at boot under the menu
  (`title_screen=0`). Esc in a session opens the in-game menu (Resume, Settings, Disconnect, Quit); the game never
  sees Esc (window messages, raw input, DirectInput), and while the menu is up the game gets no mouse or keyboard.
  `[client] menu=1` (replaces `html_menu`).
* **Disconnect can unload the world** (development, `[game] disconnect_unload=1`): the session restarts in-process (the
  game's own level-load path) into an empty session - player, puppets, horses, blips and scripts gone - and joining
  loads a new one (the player owner's handle generation is read at spawn, so the player is created in the new
  session). Not default yet: the empty session's terrain stays drawn behind the menu (re/NOTES.md "Disconnect").
  Default: the world stays loaded behind the main menu.
* `ui_exec.txt` (debug): lines already in the file when the game starts are no longer run.
* Fixed: after joining from inside the server browser, the in-game pause menu kept Options and Quit hidden.
* **Native menus, each entry its own view:** Options and the server browser open as their own view in the menu column
  (OPTIONS / SERVER BROWSER with their tabs under it; the Options tabs were not visible before). The in-game menu is
  Resume, Options, Disconnect, Quit (Map left the list); off a server: Server Browser, Direct Connect, Options, Quit.
* **Options for multiplayer:** the single-player tabs (Gameplay: Golden Guns, Autosave; Extras: cheats) are gone; a
  MULTIPLAYER tab (first) sets nickname, nametags, nametag distance and chat ([net] name, [client] nametags /
  nametag_distance / chat in rdmp.ini, applied at once). fw_api v12 (get_settings / set_settings).
* **Direct Connect** (host:port, port 27015 if left out) and the nickname use the game's own text entry box (the cheat
  entry); a direct join is remembered under Recent like a browser join.
* **Disconnect** leaves at once (no prompt, no game restart): the world stays loaded, the player waits in limbo and
  the menu becomes the main menu; joining again gives the HUD back without a world load. Dropped by the server: the
  main menu opens.
* Fixed: quitting / ending the game loop crashed in rdmp_client.dll (the Framework client was first torn down by its
  static destructor at process exit); it is shut down when our game loop ends.
* **One native menu (MTA-style), title and pause:** Map (in game), Server Browser, Direct Connect, Settings (the
  game's Options), Disconnect (in game, with confirmation), Quit; Back on the title asks to quit, in game it closes.
  Server browser in the game's Options style: Internet / Local / Favorites / Recent, players and ping, Join /
  Favourite / Refresh; joining shows the game's loading screen. fw_api v11 (server browser, disconnect).
* **Native menus (in progress, `[game] title_screen=1`, off by default):** the game boots through its own legal pages
  and title screen (~10 s, the logo video skipped) with no world loaded; Settings and Quit are the game's own. Our
  menus are written in the game's own UI language (SCXML, like its menus in content.rpf) and loaded with them
  in-process (src/game/uifiles.cpp, files in ui/native/): Play opens the Red Dead Multiplayer main menu (Server
  Browser, Direct Connect, Multiplayer Settings, Settings, Quit) in the title's style. Server browser, direct
  connect, multiplayer settings and joining from it are next. `[debug] ui_widget_dump` / `ui_file_log` map menus.
* **New main menu** (ui/): an original Red Dead Multiplayer logo and icon set, a Play page (server browser with
  Internet / Local / Favorites / Recent, search, hide empty / locked, sortable columns, player bars, ping dots, a
  details card for the selected server, direct connect, password prompt) and a Settings page with Multiplayer plus
  **the game's own settings**: Display, Graphics, Audio, Keyboard & mouse, Gamepad, Interface. Changes are staged
  (Apply / Revert, a count per section); display and graphics changes ask to be kept within 15 s or revert.
* The game's settings are read and applied through the game's own code (src/game/gamesettings.cpp), which saves
  its own options.xml / graphicsOptions.xml as its pause menu does. fw_api v10.
* The game boots to the main menu and starts no server; RDMP.exe `--server` / `--connect host[:port]` for
  development.
* **Horses** (MTA's vehicles): `createVehicle(model, x, y, z[, rz])` (rideable animals 976..1005), `getVehicleModel`,
  `warpPedIntoVehicle`, `removePedFromVehicle`, `getPedOccupiedVehicle`, `getVehicleOccupant`, events
  `playerVehicleEnter` / `playerVehicleExit`; vehicles in getElementType / getElementsByType / get/setElementPosition /
  destroyElement. Players mount and dismount with the game's own controls (hold E, the game's animations); the rider
  syncs the horse, everyone else sees it ridden with the rider on it; a riderless horse stands still (no animal AI).
  devtools: `/horse`, `/mount`, `/dismount`. fw_api v9.
* **Remote players are driven by the game's own player code** (MTA:SA's model, `[net] puppet_mode=player`, default):
  their controller state (every on-foot input) travels with the position each tick; on the other side the puppet
  runs the same player code as the local player from those inputs, so walking, sprinting, stopping, jumping and
  climbing look exactly like the real player (src/game/puppet_player.cpp, RE in re/NOTES.md). fw_api v8.
* Fixed: walking into Armadillo station froze and crashed the game. World scripts the game starts by itself near
  certain places (after the sandbox killed the boot scripts) ran in the script VM and read story state that never
  exists here. Every frame after the spawn, any script thread is now stopped before it runs (logged once per script).
* Radar blips attached to a remote player follow the puppet the game shows (no blip stuck where the network put it).
* playerblips: smaller blips (`BLIP_SIZE`, default 1 = half the game's icon size).
* `[debug] autopilot` replays recorded controller input (traces now carry it) on one copy; `puppet_player_test`
  drives puppets from a fixed input pattern; `jump_probe` RE probes. re/gh.ps1 starts Ghidra without its launcher.
* **Puppets move with their own animation.** Measured with two game copies: pulling the puppet onto the network
  position every frame cancelled the walk animation's own motion (the feet slid ~80% of the time). The new controller
  (src/game/puppet_control.cpp) replays the remote's movement input (stick pressed / released, magnitude, gait,
  desired heading) so the puppet starts, turns, runs and stops with the engine's animations; it is pulled softly only
  for large gaps (0.6 m moving, scaled with speed; 0.3 m standing, drifted in slowly after the stop animation) and
  teleported beyond 4 m. Fixed a puppet that could freeze facing the wrong way while the remote walked on, weaving on
  straight lines, and the stutter-step on stops.
* **Netcode (client and server must match):** the movement input travels in the unreliable per-tick transform with
  the position (stick 8+8 bits, desired heading 12 bits, gait 8 bits) instead of the reliable sparse desire array, so
  a lost packet can no longer hold the input back behind the position. fw_api v7. rdmp_bot sends a walking input.
* Sync test tools: `[debug] autopilot` replays a recorded route on one copy, `tools/run_two.ps1`,
  `tools/sync_report.py` (error vs truth, best-fit delay, sideways / along, heading, pulled frames).
* Crash log names the module of every stack frame.

## 0.6.0: main menu and server browser (2026-11-21)
* The project lives in the Framework fork (github.com/Soapbosnia/Red-Dead-Multiplayer, `code/projects/rdmp`).
* **Main menu** (HTML in the Framework's CEF web UI, `ui/`): server browser with Internet (MafiaHub masterlist),
  Local (this PC, ports 27015-27020), Favorites and Recent tabs, search, ping, password prompt, direct connect; a
  Settings page (nickname, nametags, nametag distance, chat). Shown whenever the client is not on a server, back on a
  disconnect with the reason. Game options stay in the game's own pause menu (Esc).
* `[net] autoconnect` (default 0): the menu picks the server; 1 keeps the old connect-at-start development flow.
* Server: `GET /rdmp` on the game port (name from server.json `"name"`, players, max players, password) for browsers.
* Leaving a server fades to black and takes control away until the next server spawns the player.
* playerblips: a player no longer sees their own blip.

## 0.5.0: overlay, chat, nametags, structured API (2026-11-21)
* **In-game overlay:** the Framework renderer + ImGui drawn into the game's own D3D12 frames (src/client/overlay.cpp:
  Present / ExecuteCommandLists hooks, renderer brought up on the main thread once the swapchain is stable, the D3D12
  backend's Begin / End around the ImGui pass). Swapchain and queue are borrowed, never AddRef'd (the game checks that).
* **Chat box** (the Framework's): T opens it, Enter sends; server `playerChat` event (return false to veto) then a
  broadcast with the player's name; `/command` lines become the `playerCommand` event. While typing the game gets no
  keyboard (DirectInput hooks) and player control is held off.
* **Nametags** above remote players (the Framework's nametag widget: name, health bar, distance fade), drawn at the
  puppet the game shows; MTA-style setPlayerNametagText / Color / Showing.
* **Script API restructured** into shared / server / client modules (src/shared/script_api.h, src/server/api_*.cpp,
  src/client/api_*.cpp); docs/SCRIPTING.md lists every function by side. New: `dxDrawText / Rectangle / Line / Circle`,
  `dxGetTextWidth`, `dxGetFontHeight`, `guiGetScreenSize`, `getScreenFromWorldPosition`, `getCameraPosition`,
  `showCursor`, `getCursorPosition`, `clientClick`, `killPed`, `showGameText`, `outputChatBox` on both sides, `tocolor`,
  `toggleAllControls` (replaces setElementControl), chat and nametag functions.
* Server split: main.cpp (instance), state.cpp, api_players / elements / blips / hud.cpp.
* fw_api v6: Vector3-sized native results, text in actions (kill, HUD text), Host show_text / kill_local /
  get_camera / set_input_lock / get_remote_position. Native calls get a 64-byte result buffer (a Vector3 return used
  to overflow it).
* Natives whose registration is skipped in retail can be called through a small handler table (SET_BLIP_PRIORITY).
* Resources: spawner and wasted simplified (no tokens, no setCameraTarget), devtools gained /commands, new scoreboard.
* rdmp_bot fixed: it joined and was dropped (its download cache path was in the user profile).
* Known issue: after a death the screen keeps showing the old body until the game reads a key press (HANDOFF.md).

## 0.4.1: first build, tested in game (2026-11-21)
* Builds clean (`build.bat` -> BUILD OK): fixed the `rdmp_data` copy step (quoted `-D` arguments under VERBATIM) and a
  declaration-order error in the server; `build.ps1` puts the VS Installer folder on PATH so vcvars finds vswhere.
* **Client script events work:** `clientKey` / `clientRender` are emitted with `EmitReservedSync` (the async
  `EmitReserved` left the handlers queued, so devtools never reacted).
* **On-screen lines work:** `outputChatBox` and server chat lines use `PRINT_OBJECTIVE_B` (bottom-centre line).
  `PRINT_SMALL_B` is never accepted by the HUD in the sandbox; a new line replaces the current one after a short fade.
* `getLocalActor()` returns the engine's script Actor handle (`GET_PLAYER_ACTOR`), so `invokeNative` on it works.
* Script respawn releases the dead body (there used to be a second Marston standing next to the new player) and calls
  `CLEAR_DEATHFX`.
* devtools F8 kills with `KILL_ACTOR`, so the death animation plays.
* playerblips: `SHOW_OWN_BLIP` switch (true while testing alone).
* The old reverse-engineering hotkeys in `rdmp.dll` (F5..F11, they clashed with devtools) are off unless
  `[debug] re_keys=1`.
* `rdmp_bot` keeps its download cache in `cache/bot` next to the exe instead of the user profile.
* Verified in game: dying no longer crashes the game (tested with `KILL_ACTOR` and with health 0).
* Known issue: after a respawn the view stays on the dead body until the first key press, then cuts to the new player.

## 0.4.0 — rdmp
* Renamed the project to **rdmp / Red Dead Multiplayer**; one clean repository (`src/`, `resources/`, `re/`, `docs/`).
* One CMake build for everything on the MafiaHub Framework 44.1.1 SDK (`build.bat`); no hard-coded paths: the SDK
  cache comes from `FW_SDK_CACHE_DIR`, RDR.exe from `rdmp.ini [game] exe` (asked once), the client DLL from
  rdmp.ini's folder.
* `rdmp_client.dll` is now pumped from the game's main thread (fw_api v5) instead of its own thread; the old UDP
  relay backend is gone.
* **Client-side scripting**: client scripts from resources run in game with natives for the local player, camera,
  controls, chat, keys and `invokeNative` for any game native; `clientKey` / `clientRender` events.
* **Elements are destroyed when the resource that created them stops.**
* `setElementVisibleTo` and `visibleTo` on blips use the Framework's per-viewer replication filters.
* Server console: `players`, `tp`, `help`.
* Chat lines from the server are printed on screen (PRINT_SMALL_B; replaced in 0.4.1).
* **Fixed: the game crashed when a player died.** The wasted script's chat line reached the Framework chat box,
  which timestamps lines with `ImGui::GetTime()`; the headless client had no ImGui context. It now creates one.
* Resources: `spawner`, `wasted`, `playerblips` (random colour per player), `devtools`.
* Removed the RE probes (radar dumps, native probes) from the game DLL.
* Tools moved to `D:\Development\Tools` (`tools/setup_tools.bat`).
