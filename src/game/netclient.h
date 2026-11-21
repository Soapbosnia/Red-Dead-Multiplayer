#pragma once

namespace rdmp {

// Reads rdmp.ini [net] (enabled, server, port, name, interp_ms); if enabled, starts the UDP client thread and the
// per-frame sync (local state out, remote players in as teleported puppets).
void StartNetClient(const wchar_t* iniPath);

// Main thread: where the game draws a remote player's puppet (Y up); false while it has none (streamed out).
bool RemotePuppetPosition(unsigned long long netId, float out[3]);

}  // namespace rdmp
