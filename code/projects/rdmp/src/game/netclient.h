#pragma once

namespace rdmp {

// Reads rdmp.ini [net] (enabled, server, port, name, interp_ms); if enabled, starts the UDP client thread and the
// per-frame sync (local state out, remote players in as teleported puppets).
void StartNetClient(const wchar_t* iniPath);

// Main thread: where the game draws a remote player's puppet (Y up); false while it has none (streamed out).
bool RemotePuppetPosition(unsigned long long netId, float out[3]);

// Native menus (title mode, game main thread). NetTitleFrame starts the framework client on the front end and pumps it
// (the server browser works before any world is loaded). NetBrowserJoin remembers the server: the world load starts and
// the client connects as soon as the world is up.
void NetTitleFrame();
struct NetServer {
    char list[16];
    char host[64];
    int port, online;
    char name[96];
    int players, maxPlayers, password, pingMs, favorite;
};
void NetBrowserRefresh(const char* list);
int NetBrowserPoll(NetServer* out, int max);
void NetBrowserFavorite(const char* host, int port, const char* name, bool add);
void NetBrowserJoin(const char* host, int port);
// The pause menu's Disconnect: leave the server (the world stays, the player goes back to limbo behind the main menu).
void NetDisconnect();
// We joined a server from the menus and have not left it (or been dropped).
bool NetOnServer();
// The world was unloaded (Disconnect): remote players' puppets, horses and blips are gone with it; forget them.
void NetWorldUnloaded();
// Our game loop ended (quit or a game restart): shut the framework client down here, on the main thread, before the game
// tears itself down; left to its static destructor at DLL unload it crashed (Framework scripting module, process exit).
void NetShutdown();
// Multiplayer settings (fw_api v12; rdmp.ini through the client DLL). Get returns false while the client is not up.
struct NetSettings {
    char nickname[32];
    int nametags, nametagDistance, chat;
};
bool NetGetSettings(NetSettings* out);
void NetSetSettings(const NetSettings& in);

}  // namespace rdmp
