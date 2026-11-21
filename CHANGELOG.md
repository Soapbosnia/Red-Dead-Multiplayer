# Changelog

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
