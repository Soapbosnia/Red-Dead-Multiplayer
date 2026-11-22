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

}  // namespace rdmp
