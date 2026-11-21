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
          overlay         : D3D12 Present hook -> Framework renderer + ImGui (chat box, nametags, dx* drawing);
                            DirectInput hooks keep typing out of the game
```

* **Threading.** The Framework client is updated from the game's frame callback, so client scripts and the
  replication callbacks run on the game's main thread and may call game natives directly.
* **Players.** The owner writes its pose/locomotion every frame; the server forces poses and queues actions
  (acknowledged by sequence number) for spawn, fade, camera, control and health.
* **Blips.** A server-side `Blip` entity (icon, colour, scale, distance, priority, flags, attachment), streamed per
  viewer with `ReplicationManager` viewer filters (`visibleTo`, `setElementVisibleTo`). `rdmp.dll` reconciles the
  replicated set with the game's blip pool every frame.
* **Script API.** src/shared/script_api.h (helpers + shared functions), src/server/api_*.cpp, src/client/api_*.cpp;
  docs/SCRIPTING.md lists each function with the side(s) it exists on.
* **Resources.** The server records which resource created each element and destroys those elements when the
  resource stops.

See `re/NOTES.md` for the engine addresses behind all of this (build 0x673783F3).
