// Debug-register write watch, used to find which game code writes a field at runtime
// (e.g. who assigns the local player's actor handle). Debug builds only; see rdmp.ini [debug].
#include "watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>

#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kPlayerInfoPtr = 0x1422DAF30;
constexpr uintptr_t kPlayerActorHandle = 0x5EC;
constexpr int kMaxHits = 40;

std::atomic<uintptr_t> g_watchAddr{0};
std::atomic<int> g_hits{0};
std::atomic<bool> g_exec{false};  // execute breakpoint instead of write watch
std::atomic<bool> g_chain{false};  // after hitting the slot, re-arm on (pointer just written + offset)
std::atomic<uintptr_t> g_chainOffset{0};
std::atomic<bool> g_access{false};  // read/write watch: each distinct accessing rip logged once
std::atomic<uintptr_t> g_accessRips[128];
std::atomic<bool> g_multi{false};  // up to 4 execute breakpoints (DR0-DR3), each hit logged with its index
uintptr_t g_multiAddr[4] = {};
std::atomic<int> g_multiHits[4];
std::atomic<uintptr_t> g_execRcx[4];  // last distinct rcx values seen by the execute breakpoint
char g_label[64] = "";
PVOID g_veh = nullptr;

uintptr_t GameStart() { return GameBase(); }
uintptr_t GameEnd() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(GameBase());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(GameBase() + dos->e_lfanew);
    return GameBase() + nt->OptionalHeader.SizeOfImage;
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (g_multi.load() && (c->Dr6 & 0xF)) {
        int idx = 0;
        while (idx < 4 && !(c->Dr6 & (1ULL << idx))) ++idx;
        c->Dr6 = 0;
        c->EFlags |= 0x10000;  // RF
        const int n = ++g_multiHits[idx];
        if (n <= 8) Log("bp[%s] #%d (%p) hit %d: thread %lu rcx=%p rdx=%p rax=%p", g_label, idx, reinterpret_cast<void*>(c->Rip), n,
                        GetCurrentThreadId(), reinterpret_cast<void*>(c->Rcx), reinterpret_cast<void*>(c->Rdx), reinterpret_cast<void*>(c->Rax));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((c->Dr6 & 1) == 0) return EXCEPTION_CONTINUE_SEARCH;  // not our DR0 hit
    c->Dr6 = 0;
    if (g_access.load()) {  // data breakpoints trap after the access: Rip is the next instruction
        for (auto& slot : g_accessRips) {
            uintptr_t seen = slot.load();
            if (seen == c->Rip) return EXCEPTION_CONTINUE_EXECUTION;
            if (seen == 0 && slot.compare_exchange_strong(seen, c->Rip)) break;
            if (seen == c->Rip) return EXCEPTION_CONTINUE_EXECUTION;
        }
        CONTEXT ctx = *c;
        char line[256];
        int len = sprintf_s(line, "access[%s]: thread %lu next rip %p via", g_label, GetCurrentThreadId(), reinterpret_cast<void*>(c->Rip));
        const uintptr_t lo = GameStart(), hi = GameEnd();
        for (int frame = 0; frame < 5 && ctx.Rip && len > 0 && len < 200; ++frame) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
            if (ctx.Rip >= lo && ctx.Rip < hi) len += sprintf_s(line + len, sizeof(line) - len, " %p", reinterpret_cast<void*>(ctx.Rip));
        }
        Log("%s", line);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // This thread's DR0 type (RW0 bits 16-17): 00 = execute. A stale execute breakpoint (thread not yet re-armed
    // after switching to a write watch) must be resumed with RF and not counted.
    const bool isExecHit = ((c->Dr7 >> 16) & 3) == 0;
    if (isExecHit) c->EFlags |= 0x10000;
    if (isExecHit != g_exec.load()) return EXCEPTION_CONTINUE_EXECUTION;

    if (g_chain.load() && !g_exec.load()) {
        // The watched slot now holds a pointer we care about: watch (pointer + offset) instead, on this same
        // thread, through the faulting context - so the very next write there is caught synchronously.
        const uintptr_t base = *reinterpret_cast<uintptr_t*>(g_watchAddr.load());
        if (base > 0x10000) {
            const uintptr_t next = (base + g_chainOffset.load()) & ~static_cast<uintptr_t>(7);
            c->Dr0 = next;
            c->Dr7 = (c->Dr7 & ~0x3ull) | 0x1ull;
            c->Dr7 = (c->Dr7 & ~(0x3ull << 16)) | (0x1ull << 16);
            c->Dr7 = (c->Dr7 & ~(0x3ull << 18)) | (0x3ull << 18);
            g_watchAddr.store(next);
            g_chain.store(false);
            g_hits.store(0);
            sprintf_s(g_label, sizeof(g_label), "chained(ptr+0x%llx)",
                      static_cast<unsigned long long>(g_chainOffset.load()));
            Log("watch[chain]: re-armed on %p (base %p + 0x%llx)", reinterpret_cast<void*>(next),
                reinterpret_cast<void*>(base), static_cast<unsigned long long>(g_chainOffset.load()));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const int n = ++g_hits;
    if (g_exec.load()) {
        c->EFlags |= 0x10000;  // RF: resume without re-triggering the execute breakpoint
        bool known = false;
        for (auto& r : g_execRcx) known = known || r.load() == c->Rcx;
        if (!known) {
            for (auto& r : g_execRcx) {
                uintptr_t expected = 0;
                if (r.compare_exchange_strong(expected, c->Rcx)) break;
            }
        }
        if (n <= kMaxHits || (n & 0x3FF) == 0) {
            const uintptr_t ret = *reinterpret_cast<uintptr_t*>(c->Rsp);
            Log("exec[%s] hit %d: thread %lu rcx=%p rdx=%p r8=%p ret=%p", g_label, n, GetCurrentThreadId(),
                reinterpret_cast<void*>(c->Rcx), reinterpret_cast<void*>(c->Rdx), reinterpret_cast<void*>(c->R8),
                reinterpret_cast<void*>(ret));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (n <= kMaxHits) {
        const uintptr_t addr = g_watchAddr.load();
        const uint32_t value = *reinterpret_cast<volatile uint32_t*>(addr);
        // Trap fires after the write: Rip is the instruction after the writer.
        Log("watch[%s] hit %d: thread %lu wrote 0x%08x (next rip=%p)", g_label, n, GetCurrentThreadId(), value,
            reinterpret_cast<void*>(c->Rip));
        // Walk the game's own stack from the faulting context.
        CONTEXT ctx = *c;
        const uintptr_t lo = GameStart(), hi = GameEnd();
        for (int frame = 0; frame < 16 && ctx.Rip; ++frame) {
            if (ctx.Rip >= lo && ctx.Rip < hi) Log("   #%d %p", frame, reinterpret_cast<void*>(ctx.Rip));
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {  // leaf function: return address is at [rsp]
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher,
                                 nullptr);
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyToThread(DWORD tid, uintptr_t addr) {
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c) && g_multi.load()) {
            c.Dr0 = g_multiAddr[0];
            c.Dr1 = g_multiAddr[1];
            c.Dr2 = g_multiAddr[2];
            c.Dr3 = g_multiAddr[3];
            c.Dr7 = (c.Dr7 & ~0xFFFF00FFULL);  // execute, len 0 for all four
            for (int i = 0; i < 4; ++i)
                if (g_multiAddr[i]) c.Dr7 |= 1ULL << (2 * i);  // Li
            SetThreadContext(th, &c);
        } else if (GetThreadContext(th, &c)) {
            c.Dr0 = addr;
            // L0=1 (bit 0); write watch: RW0=01 (bits 16-17), LEN0=11 4 bytes (bits 18-19); execute: RW0=00, LEN0=00;
            // access watch: RW0=11
            c.Dr7 = (c.Dr7 & ~0xF0003ULL) | 1ULL |
                    (g_exec.load() ? 0ULL : (((g_access.load() ? 3ULL : 1ULL) << 16) | (3ULL << 18)));
            SetThreadContext(th, &c);
        }
        ResumeThread(th);
    }
    CloseHandle(th);
}

DWORD WINAPI ArmThread(LPVOID) {
    const uintptr_t addr = g_watchAddr.load();
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    int count = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        ApplyToThread(te.th32ThreadID, addr);
        ++count;
    }
    CloseHandle(snap);
    Log("watch[%s]: armed on %p across %d threads", g_label, reinterpret_cast<void*>(addr), count);
    return 0;
}

}  // namespace

void ArmWriteWatchInline(uintptr_t address, const char* label, uintptr_t chainOffset) {
    const uintptr_t watched = address & ~static_cast<uintptr_t>(7);
    strncpy_s(g_label, label, _TRUNCATE);
    g_watchAddr.store(watched);
    g_exec.store(false);
    g_hits.store(0);
    g_chainOffset.store(chainOffset);
    g_chain.store(chainOffset != 0);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    const HANDLE thread = GetCurrentThread();
    if (!GetThreadContext(thread, &ctx)) {
        Log("watch[%s]: synchronous arm failed (error %lu)", label, GetLastError());
        return;
    }
    ctx.Dr0 = watched;
    // L0 = 1, RW0 = 01 (write), LEN0 = 11 (4 bytes)
    ctx.Dr7 = (ctx.Dr7 & ~0xFull) | 0x1ull;
    ctx.Dr7 = (ctx.Dr7 & ~(0x3ull << 16)) | (0x1ull << 16);
    ctx.Dr7 = (ctx.Dr7 & ~(0x3ull << 18)) | (0x3ull << 18);
    if (!SetThreadContext(thread, &ctx)) {
        Log("watch[%s]: synchronous DR0 write failed (error %lu)", label, GetLastError());
        return;
    }
    Log("watch[%s]: armed synchronously on %p (this thread)", label, reinterpret_cast<void*>(watched));
}

void ArmWriteWatch(uintptr_t address, const char* label) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
    strcpy_s(g_label, label);
    g_exec = false;
    g_access = false;
    g_watchAddr = address;
    g_hits = 0;
    HANDLE t = CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void ArmExecWatch(uintptr_t address, const char* label) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
    strcpy_s(g_label, label);
    g_exec = true;
    g_access = false;
    g_watchAddr = address;
    g_hits = 0;
    HANDLE t = CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

int WatchHitCount() { return g_hits.load(); }

void ArmAccessWatch(uintptr_t address, const char* label) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
    strcpy_s(g_label, label);
    for (auto& r : g_accessRips) r.store(0);
    g_exec = false;
    g_multi = false;
    g_access = true;
    g_watchAddr = address & ~static_cast<uintptr_t>(3);
    g_hits = 0;
    HANDLE t = CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void ArmExecBreakpoints(const uintptr_t* addrs, int n, const char* label) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
    strcpy_s(g_label, label);
    for (int i = 0; i < 4; ++i) {
        g_multiAddr[i] = i < n ? addrs[i] : 0;
        g_multiHits[i] = 0;
    }
    g_exec = false;
    g_multi = true;
    HANDLE t = CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
uintptr_t WatchExecRcx(int i) { return i >= 0 && i < 4 ? g_execRcx[i].load() : 0; }

void ArmPlayerHandleWatch() {
    const uintptr_t info = *reinterpret_cast<uintptr_t*>(GameAddr(kPlayerInfoPtr));
    if (!info) {
        Log("watch: player info doesn't exist yet");
        return;
    }
    ArmWriteWatch(info + kPlayerActorHandle, "playerActorHandle");
}

}  // namespace rdmp
