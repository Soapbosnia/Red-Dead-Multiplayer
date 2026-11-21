#pragma once

namespace rdmp {

// Reads rdmp.ini [net] (enabled, server, port, name, interp_ms); if enabled, starts the UDP client thread and the
// per-frame sync (local state out, remote players in as teleported puppets).
void StartNetClient(const wchar_t* iniPath);

}  // namespace rdmp
