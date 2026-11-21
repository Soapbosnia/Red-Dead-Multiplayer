# Red Dead Multiplayer (rdmp)

A sandbox multiplayer mod for **Red Dead Redemption (PC)** in the spirit of Multi Theft Auto: a dedicated server,
resources written in JavaScript (server *and* client side), and an MTA-style scripting API (`spawnPlayer`,
`createBlipAttachedTo`, `setElementVisibleTo`, ...). Networking, replication, resources and scripting come from the
[MafiaHub Framework](https://github.com/MafiaHub/Framework) (used with the permission of MafiaHub).

## Layout

| Path | What |
|---|---|
| `src/game/` | `rdmp.dll`, loaded into RDR.exe: sandbox boot, natives, remote-player puppets, radar blips, the Host table |
| `src/client/` | `rdmp_client.dll`: the Framework client (networking, replication, client scripting), pumped by `rdmp.dll` every frame |
| `src/server/` | `RDMPServer.exe`: Framework server + server scripting API |
| `src/launcher/` | `RDMP.exe`: starts the local server (optional) and RDR.exe, loads `rdmp.dll` |
| `src/bot/` | `rdmp_bot.exe`: headless fake player for testing a server |
| `src/shared/` | entities (player, blip), blip icon table, the `rdmp.dll` ↔ `rdmp_client.dll` C API (`fw_api.h`) |
| `ui/` | the main menu (HTML/CSS/JS): server browser and settings |
| `resources/` | scripts: `spawner`, `wasted`, `playerblips`, `scoreboard`, `devtools` |
| `config/rdmp.ini` | default settings (copied to `build/bin` on the first build) |
| `re/` | reverse-engineering notes, the native table, Ghidra scripts and helpers |
| `tools/` | one-off setup scripts |
| `docs/` | scripting API and architecture |

## Build

Needs Visual Studio 2022+ (or Build Tools) with *Desktop development with C++* and *C++ CMake tools*.
The Framework SDK and its vcpkg checkout live in `FW_SDK_CACHE_DIR` (set by `tools/setup_tools.bat`;
default `Tools\MafiaHub` next to the folder holding this repository) and are downloaded there on the first configure if missing.

```
build.bat            configure + build everything into build\bin   (log: build\build.log)
build.bat clean      from scratch
build.bat RDMPServer one target
```

## Main menu

The game opens on the main menu: server browser (Internet / Local / Favorites / Recent, direct connect) and the
Multiplayer settings (nickname, nametags, chat). It is plain HTML/CSS/JS in `ui/`, shown in the Framework's web UI;
favourites and recent servers are kept in `servers.json`, settings in `rdmp.ini`. A server's browser name is
`"name"` in its `server.json`.

## Run

`run.bat` (or `build\bin\RDMP.exe`). The first time it asks for `RDR.exe` and remembers it in `build\bin\rdmp.ini`
(`[game] exe`). The game opens the main menu. For development, `RDMP.exe --server --connect 127.0.0.1` also starts
`RDMPServer.exe` and joins it at once (`tools/run_two.ps1` does that for two copies).
Logs: `build\bin\rdmp.log` (game side) and `build\bin\logs\` (Framework).

Several copies on one PC: `RDMP.exe --instance 2 --name Bob`.

## Scripting

A resource is a folder in `resources/` with a `package.json`:

```json
{ "name": "myscript", "mafiahub": { "server": "server.js", "client": "client.js" } }
```

Client scripts are sent to players by the server automatically. Every element a resource creates is destroyed when
that resource stops. See [docs/SCRIPTING.md](docs/SCRIPTING.md).
