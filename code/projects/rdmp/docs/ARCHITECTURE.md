# Architecture

```
RDMP.exe ──starts──► RDMPServer.exe (optional, local)            Framework server + server scripts
   │
   └─starts RDR.exe suspended, LoadLibrary rdmp.dll, resume
                         │
        rdmp.dll  (static CRT, no Framework code)
          boot / sandbox  : sp_idle root script, menus + legals skipped, player created, limbo until spawned
          natives         : the game's native registry (hash → handler), NativeCall
          host            : fw::Host table (log, invoke_native, local player, fade, camera, controls, print_text)
          netclient       : per frame (main thread): FwStep() → rdmp_client.dll; puppets for remote players;
                            server actions (spawn, fade, camera, control, health); blips → the game's blip pool
                         │  fw_api.h (C ABI, versioned)
        rdmp_client.dll  (/MD, Framework::Client, loaded from rdmp.ini's folder)
          FwClient        : Framework client instance (headless: no window of its own, ImGui context only for the
                            chat model), replication of Player and Blip entities
          client scripts  : run inside Update() on the game's main thread; natives call back through fw::Host
          frontend        : main menu (ui/ HTML in the Framework's CEF web UI): server browser + settings, shown
                            while not on a server; server_query fetches GET /rdmp from servers and the masterlist
          overlay         : D3D12 Present hook -> Framework renderer + ImGui (chat box, nametags, dx* drawing);
                            DirectInput hooks keep typing out of the game
```

* **Threading.** The Framework client is updated from the game's frame callback, so client scripts and the
  replication callbacks run on the game's main thread and may call game natives directly.
* **Players.** The owner writes its pose and movement input every frame; the server forces poses and queues actions
  (acknowledged by sequence number) for spawn, fade, camera, control and health.
* **Remote players are driven by the game's player code** (src/game/puppet_player.cpp, MTA:SA's context switch
  applied to RDR; `[net] puppet_mode=player`, the default). Each transform carries the sender's controller state
  (the 30 on-foot input values of its control object) and desired heading. Every puppet has a player-info and an
  on-foot control object of its own (copies of the local player's, its input records filled from the network), the
  game's player-control components, and its MarstonBehave (the player behaviour every actor's behaviour set holds)
  made its active behaviour. Around the puppet's control update and its behaviour update the global player-info
  pointer points at the puppet's, so the game's own player code walks, sprints, jumps and climbs it exactly like
  the local player. Position is corrected only beyond 1 m (more with speed), teleported beyond 4 m. Measured
  (two copies, replayed key input): 0.31 m median while moving, 4% of moving frames pulled. Details:
  re/NOTES.md "Player input path".
* **Remote-player sync, desire mode** (`puppet_mode=desire`; MTA-style: input + position in one packet). Each transform update (unreliable, every tick)
  carries position, heading, speed and the movement input (`MoveInput`: stick, desired heading, gait), timed by the
  sender's game-frame clock. The receiver interpolates `interp_ms` behind the newest sample and drives one puppet per
  remote player (src/game/puppet_control.cpp): the puppet gets the remote's input as its locomotion desire, steered by
  the remote's desired heading plus a capped bend toward its path, so the engine's own walk / run / sprint, turn and
  stop animations move it at the remote's speed. Moving the puppet directly cancels the animation's own motion (the
  feet slide), so it is only pulled softly when the gap passes 0.6 m (scaled with speed) or the remote is on another
  level, drifted in slowly once standing (> 0.3 m), and teleported beyond 4 m. Measured with two copies
  (tools/sync_report.py): the puppet shows the remote ~0.1 s late, ~0.2-0.3 m mean error, ~5% of moving frames
  pulled. Not synced yet: jumps, vaults and climbs (no input for them in the desire; needs RE).
* **Vehicles (horses).** A server `Vehicle` entity (shared/vehicle.h: model, pose, the rider's input to it, occupant).
  Server-owned while riderless; when a player mounts (the game's own E, reported as Player::localVehicle, or
  warpPedIntoVehicle) the server sets Player::vehicle / Vehicle::occupant and makes the rider's client its owner, so
  the rider syncs it (MTA's driver sync). src/game/vehicles.cpp spawns a horse per vehicle in range; others drive
  their copy with the puppet controller from the rider's input (standing still when riderless: TASK_CLEAR at spawn and
  our locomotion override every frame, no animal AI) and mount the rider's puppet on it.
* **Blips.** A server-side `Blip` entity (icon, colour, scale, distance, priority, flags, attachment), streamed per
  viewer with `ReplicationManager` viewer filters (`visibleTo`, `setElementVisibleTo`). `rdmp.dll` reconciles the
  replicated set with the game's blip pool every frame.
* **Script API.** src/shared/script_api.h (helpers + shared functions), src/server/api_*.cpp, src/client/api_*.cpp;
  docs/SCRIPTING.md lists each function with the side(s) it exists on.
* **Resources.** The server records which resource created each element and destroys those elements when the
  resource stops.

See `re/NOTES.md` for the engine addresses behind all of this (build 0x673783F3).
