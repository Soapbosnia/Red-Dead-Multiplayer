// Boot control: which root script the game starts.
//
// FUN_1401ee380 (RE: re/NOTES.md) returns the path of the root script the game launches:
// normally "$/content/main" (story/front-end), or "$/content/sp_idle" when an internal mode flag is set.
// We replace it so the game starts the script named in rdmp.ini instead.
#include "boot.h"

#include <intrin.h>

#include <string>

#include "actors.h"
#include "events.h"
#include "natives.h"
#include "sandbox.h"
#include "log.h"
#include "netclient.h"
#include "patch.h"
#include "sandbox.h"

namespace rdmp {
namespace {

constexpr uintptr_t kSelectRootScript = 0x1401EE380;

char g_rootScript[260] = "$/content/sp_idle";

// FUN_1401ee430 asks for the root script when the in-game world load starts (after "Play").
constexpr uintptr_t kWorldLoadCaller = 0x1401EE442;

const char* SelectRootScript(void* game) {
    void* caller = _ReturnAddress();
    rdmp::Log("root script requested (game=%p, caller=%p) -> \"%s\"", game, caller, g_rootScript);
    if (reinterpret_cast<uintptr_t>(caller) == GameAddr(kWorldLoadCaller)) SandboxOnWorldLoadStarted();
    return g_rootScript;
}

std::wstring IniPath(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L'\\') + 1) + L"rdmp.ini";
}

}  // namespace

void InstallBootPatches(HMODULE self) {
    const std::wstring ini = IniPath(self);
    LoadSandboxConfig(ini.c_str());
    StartNetClient(ini.c_str());
    g_watchOnLoad = GetPrivateProfileIntW(L"debug", L"watch_player_handle", 0, ini.c_str()) != 0;
    InstallNativeHook(GetPrivateProfileIntW(L"debug", L"log_natives", 0, ini.c_str()) != 0);
    g_traceAfterPlayerCreate = GetPrivateProfileIntW(L"debug", L"trace_after_player", 0, ini.c_str()) != 0;
    if (g_watchOnLoad) Log("debug: player-handle write watch will arm on loadStart");
    // root_script: script the game boots into. Empty = leave the game's own choice alone.
    wchar_t wvalue[260] = {};
    GetPrivateProfileStringW(L"boot", L"root_script", L"$/content/sp_idle", wvalue, 260, ini.c_str());
    char value[260] = {};
    WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, value, sizeof(value), nullptr, nullptr);
    if (value[0] == '\0') {
        Log("boot: root_script empty in rdmp.ini, not patching the root script");
        return;
    }
    strcpy_s(g_rootScript, value);
    if (ReplaceFunction(GameAddr(kSelectRootScript), reinterpret_cast<const void*>(&SelectRootScript))) {
        Log("boot: root script forced to \"%s\"", g_rootScript);
    } else {
        Log("boot: failed to patch the root-script selector (error %lu)", GetLastError());
    }
}

}  // namespace rdmp
