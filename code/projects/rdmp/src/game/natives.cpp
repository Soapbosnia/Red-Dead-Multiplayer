// Script VM native-call hook (debugging: see which natives the game's own scripts call).
//
// RE (re/NOTES.md "Script VM"): the interpreter calls natives at 0x1401E4353:
//   1401e4324  mov rax,[rax+0x38]      ; program's native table
//   1401e4328  mov rdx,[rax+rdx*8]     ; fn = table[index]
//   1401e4347  49 0f 44 c0             cmovz rax,r8
//   1401e434b  48 8d 4d 80             lea   rcx,[rbp-0x80]   ; ctx
//   1401e434f  48 89 45 80             mov   [rbp-0x80],rax
//   1401e4353  ff d2                   call  rdx
//   1401e4355  ...
// 1401e4347..1401e4354 (14 bytes) become "jmp trampoline"; the trampoline replays the three moved instructions,
// calls NativeHook(ctx, fn) (which calls fn) and jumps back to 1401e4355.
//
// Calling natives ourselves does not go through here: see scriptnatives.h (the registry + NativeContext).
#include "natives.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "scriptnatives.h"

namespace rdmp {
namespace {

constexpr uintptr_t kPatchAt = 0x1401E4347;
constexpr uintptr_t kResumeAt = 0x1401E4355;
constexpr uint8_t kExpected[14] = {0x49, 0x0F, 0x44, 0xC0, 0x48, 0x8D, 0x4D, 0x80,
                                   0x48, 0x89, 0x45, 0x80, 0xFF, 0xD2};

using NativeFn = void (*)(void* ctx);

bool g_log = false;
std::atomic<uint64_t> g_calls{0};
std::mutex g_seenMutex;
std::unordered_set<uintptr_t>* g_seen = nullptr;
uint32_t g_order = 0;
std::atomic<int> g_traceLeft{0};

void NativeHook(void* ctx, NativeFn fn) {
    ++g_calls;
    const uintptr_t f = reinterpret_cast<uintptr_t>(fn);
    if (g_log) {
        std::lock_guard lock(g_seenMutex);
        if (g_seen->insert(f).second) {
            Log("native #%u first call: %p (ctx=%p)", ++g_order, reinterpret_cast<void*>(f), ctx);
        }
    }
    if (g_traceLeft > 0) {
        --g_traceLeft;
        const auto* c = static_cast<const NativeContext*>(ctx);
        uint64_t a[6] = {};
        const uint32_t n = c->argc < 6 ? c->argc : 6;
        if (c->args && IsReadable(reinterpret_cast<uintptr_t>(c->args), n * 8)) {
            for (uint32_t i = 0; i < n; ++i) a[i] = c->args[i];
        }
        fn(ctx);
        const uint32_t r = c->result && IsReadable(reinterpret_cast<uintptr_t>(c->result), 4)
                               ? *static_cast<const uint32_t*>(c->result)
                               : 0;
        Log("trace %p argc=%u args=[%llx %llx %llx %llx %llx %llx] -> %08x", reinterpret_cast<void*>(f), c->argc,
            a[0], a[1], a[2], a[3], a[4], a[5], r);
        return;
    }
    fn(ctx);
}

bool FitsRel32(uintptr_t from, uintptr_t to) {
    const intptr_t d = static_cast<intptr_t>(to) - static_cast<intptr_t>(from);
    return d > -0x7FFF0000LL && d < 0x7FFF0000LL;
}

// An executable page within +-2GB of `site` (for rel32 jumps): the first free region in the window.
uint8_t* AllocNear(uintptr_t site) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = site > 0x7FF00000ULL ? site - 0x7FF00000ULL : gran;
    const uintptr_t hi = site + 0x7FF00000ULL;
    uintptr_t p = lo;
    while (p < hi) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const uintptr_t cand = (base + gran - 1) & ~(gran - 1);
            if (cand + 0x1000 <= end && FitsRel32(site, cand) && FitsRel32(cand + 0x1000, site)) {
                void* r = VirtualAlloc(reinterpret_cast<void*>(cand), 0x1000, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE);
                if (r && FitsRel32(site, reinterpret_cast<uintptr_t>(r))) return static_cast<uint8_t*>(r);
                if (r) VirtualFree(r, 0, MEM_RELEASE);
            }
        }
        p = end;
    }
    return nullptr;
}

}  // namespace

uint64_t NativeCallCount() { return g_calls.load(); }

void TraceNextNativeCalls(int count) {
    g_traceLeft = count;
    Log("natives: tracing the next %d native calls", count);
}

void InstallNativeHook(bool logNatives) {
    g_log = logNatives;
    g_seen = new std::unordered_set<uintptr_t>();
    const uintptr_t at = GameAddr(kPatchAt);
    if (std::memcmp(reinterpret_cast<void*>(at), kExpected, sizeof(kExpected)) != 0) {
        Log("natives: call site bytes differ, not hooking");
        return;
    }
    uint8_t* t = AllocNear(at);
    if (!t || !FitsRel32(at + 5, reinterpret_cast<uintptr_t>(t))) {
        Log("natives: no trampoline memory within 2GB of the call site (got %p), not hooking", t);
        return;
    }
    size_t n = 0;
    std::memcpy(t + n, kExpected, 12);  // cmovz rax,r8 ; lea rcx,[rbp-0x80] ; mov [rbp-0x80],rax
    n += 12;
    t[n++] = 0x48; t[n++] = 0xB8;       // mov rax, imm64 (NativeHook)
    const uint64_t hook = reinterpret_cast<uint64_t>(&NativeHook);
    std::memcpy(t + n, &hook, 8);
    n += 8;
    t[n++] = 0xFF; t[n++] = 0xD0;       // call rax   (rcx = ctx, rdx = fn, same stack alignment as the original call)
    t[n++] = 0xE9;                      // jmp rel32 back
    const int32_t back = static_cast<int32_t>(GameAddr(kResumeAt) - (reinterpret_cast<uintptr_t>(t) + n + 4));
    std::memcpy(t + n, &back, 4);
    n += 4;
    FlushInstructionCache(GetCurrentProcess(), t, n);

    uint8_t patch[14];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(t) - (at + 5));
    std::memcpy(patch + 1, &to, 4);
    if (WriteCode(at, patch, sizeof(patch))) {
        Log("natives: VM native call hooked (trampoline %p, logging %s)", t, logNatives ? "on" : "off");
    } else {
        Log("natives: failed to patch the call site (error %lu)", GetLastError());
    }
}

}  // namespace rdmp
