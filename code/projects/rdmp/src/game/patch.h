#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace rdmp {

// RDR.exe loads at its preferred base, so Ghidra addresses are used as-is (see NOTES.md).
inline uintptr_t GameBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
inline uintptr_t GameAddr(uintptr_t ghidraAddr) { return GameBase() + (ghidraAddr - 0x140000000ULL); }

// Writes bytes into game code (temporarily making it writable).
inline bool WriteCode(uintptr_t at, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    return true;
}

// Replaces the function at `at` with an absolute jump to `target` (mov rax, imm64; jmp rax = 12 bytes).
// The original function is not callable afterwards: use only for functions we fully reimplement.
inline bool ReplaceFunction(uintptr_t at, const void* target) {
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const uint64_t t = reinterpret_cast<uint64_t>(target);
    std::memcpy(code + 2, &t, sizeof(t));
    return WriteCode(at, code, sizeof(code));
}


// Detours a function whose first `len` bytes (`prologue`, must match and be position independent, len >= 12) are
// copied to a trampoline; returns the trampoline (call it to run the original) or nullptr.
inline void* DetourPic(uintptr_t at, const uint8_t* prologue, size_t len, const void* hook) {
    if (len < 12 || std::memcmp(reinterpret_cast<void*>(at), prologue, len) != 0) return nullptr;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    std::memcpy(t, prologue, len);
    t[len] = 0xFF; t[len + 1] = 0x25; std::memset(t + len + 2, 0, 4);  // jmp [rip+0]
    const uint64_t back = at + len;
    std::memcpy(t + len + 6, &back, sizeof(back));
    uint8_t code[64];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0x48; code[1] = 0xB8;  // mov rax, hook; jmp rax
    const uint64_t h = reinterpret_cast<uint64_t>(hook);
    std::memcpy(code + 2, &h, sizeof(h));
    code[10] = 0xFF; code[11] = 0xE0;
    if (!WriteCode(at, code, len)) return nullptr;
    return t;
}
// Replaces the IAT slot of `func` imported from `dll` in the main module; returns the original pointer.
inline void* PatchImport(const char* dll, const char* func, void* hook) {
    const auto base = reinterpret_cast<uint8_t*>(GameBase());
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        auto* orig = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
            void* old = reinterpret_cast<void*>(thunk->u1.Function);
            DWORD prot;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            thunk->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), prot, &prot);
            return old;
        }
    }
    return nullptr;
}

}  // namespace rdmp
