# Changelog

## Unreleased: remote-player sync rework
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
