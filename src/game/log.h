#pragma once
#include <windows.h>

namespace rdmp {

// Opens rdmp.log next to the DLL (truncates it on each launch); instance N >= 2 logs to rdmp_<N>.log.
void LogInit(HMODULE self);
// printf-style line logger: thread-safe, flushes every line, prefixes a timestamp.
void Log(const char* fmt, ...);

// Game instance number for running several copies on one PC (RDMP.exe --instance N sets the RDMP_INSTANCE
// environment variable the game inherits). 1 = the default / only instance.
int InstanceId();

}  // namespace rdmp
