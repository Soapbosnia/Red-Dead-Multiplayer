// Instance diagnostics (game copy N >= 2, or rdmp.ini [debug] instance_trace=1): logs the game's own calls to the
// single-instance / exit APIs it imports (CreateMutexA/W, FindWindowA, ExitProcess, TerminateProcess) with the
// caller's module + section, so we can tell whether a second copy is stopped by game code. Pass-through only.
#include "instancediag.h"

#include <intrin.h>

#include <cstdio>

#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

using CreateMutexA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
using CreateMutexW_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using FindWindowA_t = HWND(WINAPI*)(LPCSTR, LPCSTR);
using ExitProcess_t = void(WINAPI*)(UINT);
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);

CreateMutexA_t g_createMutexA = nullptr;
CreateMutexW_t g_createMutexW = nullptr;
FindWindowA_t g_findWindowA = nullptr;
ExitProcess_t g_exitProcess = nullptr;
TerminateProcess_t g_terminateProcess = nullptr;

// "RDR.exe!.text+0x1234 (0x140001234)" or "<module>+0x..." for the caller address.
void Where(const void* addr, char* out, size_t outLen) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &mod) || !mod) {
        sprintf_s(out, outLen, "unknown (%p)", addr);
        return;
    }
    char name[MAX_PATH];
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    const uintptr_t rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize) {
            char sname[9] = {};
            memcpy(sname, sec->Name, 8);
            sprintf_s(out, outLen, "%s!%s+0x%llx (%p)", base, sname, static_cast<unsigned long long>(rva - sec->VirtualAddress), addr);
            return;
        }
    }
    sprintf_s(out, outLen, "%s+0x%llx (%p)", base, static_cast<unsigned long long>(rva), addr);
}

// The game's single-instance guard (RE: main FUN_14007f800 -> FUN_140eb8e40 creates this mutex and returns 1 from main
// when it already exists). Extra copies started with RDMP.exe --instance N (N >= 2) are let through, like SDMP's
// UFG_GameMutex_SD2 bypass.
constexpr char kGameInstanceMutex[] = "esdksfkfhkhdfj2255984x";

HANDLE WINAPI HookCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
    HANDLE h = g_createMutexA(sa, owner, name);
    DWORD err = GetLastError();
    char where[160];
    Where(_ReturnAddress(), where, sizeof(where));
    Log("instance: CreateMutexA(\"%s\") -> %p%s from %s", name ? name : "(null)", h, err == ERROR_ALREADY_EXISTS ? " ALREADY_EXISTS" : "", where);
    if (err == ERROR_ALREADY_EXISTS && name && strcmp(name, kGameInstanceMutex) == 0 && InstanceId() > 1) {
        err = ERROR_SUCCESS;
        Log("instance: game copy %d: single-instance guard let through", InstanceId());
    }
    SetLastError(err);
    return h;
}

HANDLE WINAPI HookCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name) {
    HANDLE h = g_createMutexW(sa, owner, name);
    const DWORD err = GetLastError();
    char where[160];
    Where(_ReturnAddress(), where, sizeof(where));
    Log("instance: CreateMutexW(\"%ls\") -> %p%s from %s", name ? name : L"(null)", h, err == ERROR_ALREADY_EXISTS ? " ALREADY_EXISTS" : "", where);
    SetLastError(err);
    return h;
}

HWND WINAPI HookFindWindowA(LPCSTR cls, LPCSTR title) {
    HWND w = g_findWindowA(cls, title);
    char where[160];
    Where(_ReturnAddress(), where, sizeof(where));
    Log("instance: FindWindowA(\"%s\", \"%s\") -> %p from %s", cls && HIWORD(reinterpret_cast<uintptr_t>(cls)) ? cls : "(atom/null)",
        title ? title : "(null)", w, where);
    return w;
}

void WINAPI HookExitProcess(UINT code) {
    char where[160];
    Where(_ReturnAddress(), where, sizeof(where));
    Log("instance: ExitProcess(%u) from %s", code, where);
    g_exitProcess(code);
}

BOOL WINAPI HookTerminateProcess(HANDLE proc, UINT code) {
    char where[160];
    Where(_ReturnAddress(), where, sizeof(where));
    Log("instance: TerminateProcess(%s, %u) from %s", proc == GetCurrentProcess() || GetProcessId(proc) == GetCurrentProcessId() ? "self" : "other",
        code, where);
    return g_terminateProcess(proc, code);
}

}  // namespace

void InstallInstanceDiagnostics() {
    g_createMutexA = reinterpret_cast<CreateMutexA_t>(PatchImport("KERNEL32.dll", "CreateMutexA", reinterpret_cast<void*>(&HookCreateMutexA)));
    g_createMutexW = reinterpret_cast<CreateMutexW_t>(PatchImport("KERNEL32.dll", "CreateMutexW", reinterpret_cast<void*>(&HookCreateMutexW)));
    g_findWindowA = reinterpret_cast<FindWindowA_t>(PatchImport("USER32.dll", "FindWindowA", reinterpret_cast<void*>(&HookFindWindowA)));
    g_exitProcess = reinterpret_cast<ExitProcess_t>(PatchImport("KERNEL32.dll", "ExitProcess", reinterpret_cast<void*>(&HookExitProcess)));
    g_terminateProcess = reinterpret_cast<TerminateProcess_t>(PatchImport("KERNEL32.dll", "TerminateProcess", reinterpret_cast<void*>(&HookTerminateProcess)));
    Log("instance diagnostics: CreateMutexA %s, CreateMutexW %s, FindWindowA %s, ExitProcess %s, TerminateProcess %s",
        g_createMutexA ? "hooked" : "-", g_createMutexW ? "hooked" : "-", g_findWindowA ? "hooked" : "-", g_exitProcess ? "hooked" : "-",
        g_terminateProcess ? "hooked" : "-");
}

}  // namespace rdmp
