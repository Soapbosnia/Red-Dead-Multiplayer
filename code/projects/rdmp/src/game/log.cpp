#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace rdmp {

static FILE* g_log = nullptr;
static std::mutex g_logMutex;

int InstanceId() {
    static const int id = [] {
        wchar_t v[16] = {};
        const int n = GetEnvironmentVariableW(L"RDMP_INSTANCE", v, 16) ? _wtoi(v) : 1;
        return n >= 1 ? n : 1;
    }();
    return id;
}

void LogInit(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t name[32];
    if (InstanceId() > 1) swprintf_s(name, L"rdmp_%d.log", InstanceId());
    else wcscpy_s(name, L"rdmp.log");
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), name);
    // _SH_DENYWR: other processes may read the log while the game is running. "N" = not inheritable: the game's
    // NVIDIA DLSS module starts nvngx_update.exe helpers that outlive the game and would keep an inherited handle (and so
    // the log) locked for the next launch.
    g_log = _wfsopen(path, L"wN", _SH_DENYWR);
    if (!g_log && slash) {  // another copy of the game holds it: fall back to a per-process log
        swprintf_s(name, L"rdmp_pid%lu.log", GetCurrentProcessId());
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), name);
        g_log = _wfsopen(path, L"wN", _SH_DENYWR);
    }
}

void Log(const char* fmt, ...) {
    std::lock_guard lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

}  // namespace rdmp
