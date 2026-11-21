#pragma once
#include <windows.h>

#include <cstdint>

namespace rdmp {

// True if [addr, addr+len) is committed, readable memory.
// IMPORTANT: never probe game memory with __try/__except. RDR's crash reporter is a vectored exception handler
// that treats ANY first-chance access violation as a crash (it pops RDRMessage.exe), even one we'd catch ourselves.
inline bool IsReadable(uintptr_t addr, size_t len = 8) {
    if (addr < 0x10000) return false;
    uintptr_t p = addr;
    const uintptr_t end = addr + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

}  // namespace rdmp
