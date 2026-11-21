// RDMP launcher: starts RDR.exe suspended, injects rdmp.dll, then resumes the game.
//
// Usage: RDMP.exe [--game <path to RDR.exe>] [--dll <path>] [--wait] [--instance N] [--name X] [--connect host[:port]]
//                  [--server] [-- <args passed to RDR.exe>]
// Without options the game opens the main menu. --connect joins that server at once (development); --server also
// starts the local RDMPServer.exe first (or rdmp.ini [net] start_local_server=1).
//   --instance N  run an extra copy of the game on this PC (N >= 2: own rdmp_N.log, name + N, spawn spot)
//   --name X      multiplayer nickname (overrides rdmp.ini [net] name)
// rdmp.dll and rdmp.ini are taken from the launcher's folder. RDR.exe: --game, else rdmp.ini [game] exe, else the
// launcher's folder and its parents, else a file picker (the choice is saved to rdmp.ini [game] exe).
#include <windows.h>
#include <commdlg.h>
#include <tlhelp32.h>
#pragma comment(lib, "comdlg32.lib")

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void Fatal(const std::wstring& text) {
    fwprintf(stderr, L"[RDMP] %s\n", text.c_str());
    MessageBoxW(nullptr, text.c_str(), L"Red Dead Multiplayer", MB_OK | MB_ICONERROR);
}

static void Fail(const wchar_t* what, DWORD err = GetLastError()) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    fwprintf(stderr, L"[RDMP] %s failed (error %lu): %s\n", what, err, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

static fs::path ModuleDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static fs::path FindGameExe(const fs::path& start) {
    fs::path dir = start;
    for (int i = 0; i < 5 && !dir.empty(); ++i) {
        fs::path candidate = dir / L"RDR.exe";
        if (fs::exists(candidate)) return candidate;
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// rdmp.ini [game] exe, else RDR.exe next to / above the launcher, else ask (and remember the answer).
static fs::path ResolveGameExe(const fs::path& ini) {
    wchar_t v[MAX_PATH] = {};
    GetPrivateProfileStringW(L"game", L"exe", L"", v, MAX_PATH, ini.c_str());
    if (v[0] && fs::exists(v)) return v;
    if (fs::path found = FindGameExe(ModuleDir()); !found.empty()) return found;
    wchar_t file[MAX_PATH] = L"RDR.exe";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.lpstrFilter = L"Red Dead Redemption (RDR.exe)\0RDR.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Where is Red Dead Redemption's RDR.exe?";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    WritePrivateProfileStringW(L"game", L"exe", file, ini.c_str());
    return file;
}

static std::wstring Quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

static bool ProcessRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exeName) == 0;
    CloseHandle(snap);
    return found;
}

static bool g_startServer = false;  // --server

// --server or rdmp.ini [net] start_local_server=1, with server=127.0.0.1: start RDMPServer.exe (next to the launcher) in its own
// console window unless one is already running. It keeps running after the game.
static void StartLocalServer(const fs::path& ini) {
    wchar_t v[MAX_PATH];
    if (!GetPrivateProfileIntW(L"net", L"enabled", 1, ini.c_str())) return;
    if (!g_startServer && !GetPrivateProfileIntW(L"net", L"start_local_server", 0, ini.c_str())) return;
    GetPrivateProfileStringW(L"net", L"server", L"127.0.0.1", v, MAX_PATH, ini.c_str());
    if (wcscmp(v, L"127.0.0.1") != 0 && _wcsicmp(v, L"localhost") != 0) return;
    if (ProcessRunning(L"RDMPServer.exe")) {
        wprintf(L"[RDMP] local server already running\n");
        return;
    }
    const fs::path dir = ModuleDir();
    const fs::path exe = dir / L"RDMPServer.exe";
    if (!fs::exists(exe)) {
        fwprintf(stderr, L"[RDMP] local server not found: %s\n", exe.c_str());
        return;
    }
    std::wstring cmd = Quote(exe.wstring());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi)) {
        wprintf(L"[RDMP] started the local server (pid %lu)\n", pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(1500);  // let it bind before the game connects
    } else {
        Fail(L"starting RDMPServer.exe");
    }
}

// Loads `dll` into `proc` via a remote LoadLibraryW thread. Returns true on success.
static bool InjectDll(HANDLE proc, const fs::path& dll) {
    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Fail(L"VirtualAllocEx"); return false; }

    bool ok = false;
    if (!WriteProcessMemory(proc, remote, path.c_str(), bytes, nullptr)) {
        Fail(L"WriteProcessMemory");
    } else {
        // kernel32 is mapped at the same address in every process for this boot session.
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!thread) {
            Fail(L"CreateRemoteThread");
        } else {
            DWORD wait = WaitForSingleObject(thread, 20000);
            DWORD exitCode = 0;
            GetExitCodeThread(thread, &exitCode);  // low 32 bits of the HMODULE (0 = failed)
            if (wait != WAIT_OBJECT_0) {
                fwprintf(stderr, L"[RDMP] LoadLibraryW thread did not finish in 20s\n");
            } else if (exitCode == 0) {
                fwprintf(stderr, L"[RDMP] LoadLibraryW returned NULL inside the game (missing file or DLL init failed)\n");
            } else {
                ok = true;
            }
            CloseHandle(thread);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    fs::path dll = ModuleDir() / L"rdmp.dll";
    const fs::path ini = ModuleDir() / L"rdmp.ini";
    fs::path game;
    bool waitForExit = false;
    std::vector<std::wstring> passthrough;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (a == L"--game" && i + 1 < argc) game = argv[++i];
        else if (a == L"--wait") waitForExit = true;
        // Several copies of the game on one PC: the game inherits these (own log, name, spawn spot).
        else if (a == L"--instance" && i + 1 < argc) SetEnvironmentVariableW(L"RDMP_INSTANCE", argv[++i]);
        else if (a == L"--name" && i + 1 < argc) SetEnvironmentVariableW(L"RDMP_NAME", argv[++i]);
        else if (a == L"--connect" && i + 1 < argc) SetEnvironmentVariableW(L"RDMP_CONNECT", argv[++i]);
        else if (a == L"--server") g_startServer = true;
        else if (a == L"--") { for (++i; i < argc; ++i) passthrough.push_back(argv[i]); }
        else passthrough.push_back(a);
    }

    if (game.empty()) game = ResolveGameExe(ini);
    if (game.empty() || !fs::exists(game)) {
        Fatal(L"RDR.exe not found. Pass --game <path> or set [game] exe in rdmp.ini.");
        return 1;
    }
    if (!fs::exists(dll)) {
        Fatal(L"rdmp.dll not found next to RDMP.exe: " + dll.wstring());
        return 1;
    }
    game = fs::absolute(game);
    dll = fs::absolute(dll);

    std::wstring cmdline = Quote(game.wstring());
    for (auto& p : passthrough) cmdline += L" " + Quote(p);

    wprintf(L"[RDMP] game: %s\n[RDMP] dll:  %s\n", game.c_str(), dll.c_str());
    StartLocalServer(ini);

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(game.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        game.parent_path().c_str(), &si, &pi)) {
        Fail(L"CreateProcessW");
        return 1;
    }
    wprintf(L"[RDMP] started RDR.exe suspended (pid %lu)\n", pi.dwProcessId);

    if (!InjectDll(pi.hProcess, dll)) {
        Fatal(L"Injecting rdmp.dll into the game failed; the game was closed.");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    wprintf(L"[RDMP] rdmp.dll injected, resuming the game\n");

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    int rc = 0;
    if (waitForExit) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        wprintf(L"[RDMP] game exited with code 0x%lx\n", code);
        rc = static_cast<int>(code);
    }
    CloseHandle(pi.hProcess);
    return rc;
}
