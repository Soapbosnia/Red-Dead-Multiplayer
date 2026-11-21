// rdmp.dll: injected into RDR.exe by RDMP.exe before the game's main thread runs.
// For now it only verifies it's inside the RDR.exe build that was reverse engineered, and logs that.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "actors.h"
#include "boot.h"
#include "crashlog.h"
#include "debugkeys.h"
#include "events.h"
#include "instancediag.h"
#include "locomotion.h"
#include "log.h"
#include "mainthread.h"

namespace {

// Fingerprint of the analyzed build (re/NOTES.md). Addresses are only valid for this build.
constexpr uint32_t kExpectedTimestamp = 0x673783F3;  // PE TimeDateStamp, 2024-11-15
constexpr uintptr_t kPreferredBase = 0x140000000;
constexpr uintptr_t kRvaSelectRootScript = 0x1EE380;  // FUN_1401ee380: picks "$/content/main"
constexpr uint8_t kSelectRootScriptBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                              0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x0D};

uintptr_t g_base = 0;
HMODULE g_self = nullptr;

bool CheckBuild() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const uint32_t ts = nt->FileHeader.TimeDateStamp;
    rdmp::Log("RDR.exe base=0x%llx (preferred 0x%llx) sizeOfImage=0x%x timestamp=0x%08x",
               static_cast<unsigned long long>(g_base), static_cast<unsigned long long>(kPreferredBase),
               nt->OptionalHeader.SizeOfImage, ts);

    const bool tsOk = ts == kExpectedTimestamp;
    const bool bytesOk = std::memcmp(reinterpret_cast<void*>(g_base + kRvaSelectRootScript),
                                     kSelectRootScriptBytes, sizeof(kSelectRootScriptBytes)) == 0;
    rdmp::Log("build check: timestamp %s, root-script selector bytes %s", tsOk ? "OK" : "MISMATCH",
               bytesOk ? "OK" : "MISMATCH");
    return tsOk && bytesOk;
}

DWORD WINAPI InitThread(LPVOID) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    rdmp::Log("rdmp.dll loaded into pid %lu", GetCurrentProcessId());
    if (!CheckBuild()) {
        rdmp::Log("unknown RDR.exe build: staying passive (no patches applied)");
        return 0;
    }
    rdmp::Log("build matches: installing patches");
    {
        wchar_t ini[MAX_PATH];
        GetModuleFileNameW(g_self, ini, MAX_PATH);
        if (wchar_t* slash = wcsrchr(ini, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"rdmp.ini");
        if (rdmp::InstanceId() > 1 || GetPrivateProfileIntW(L"debug", L"instance_trace", 0, ini)) rdmp::InstallInstanceDiagnostics();
    }
    rdmp::InstallCrashLogger();
    rdmp::InstallBootPatches(g_self);
    rdmp::InstallActorHooks();
    rdmp::InstallMainThreadHook();
    rdmp::InstallLocomotionHook();
    rdmp::StartEventLogger();
    {
        // Old reverse-engineering hotkeys (F5..F11: loader/camera/actor probes, puppets). Off by default: F7..F9
        // belong to scripts (devtools). [debug] re_keys=1 turns them back on.
        wchar_t ini[MAX_PATH];
        GetModuleFileNameW(g_self, ini, MAX_PATH);
        if (wchar_t* slash = wcsrchr(ini, L'\\'))wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"rdmp.ini");
        if (GetPrivateProfileIntW(L"debug", L"re_keys", 0, ini)) rdmp::StartDebugKeys();
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_self = module;
        rdmp::LogInit(module);
        // The game's main thread is still suspended while we run here, so the build check
        // can run directly; later hook installation will also happen at this point.
        InitThread(nullptr);
    }
    return TRUE;
}
