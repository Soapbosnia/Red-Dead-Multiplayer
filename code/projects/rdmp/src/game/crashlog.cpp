#include "crashlog.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

std::atomic<int> g_logged{0};
constexpr int kMaxLogged = 12;

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const CONTEXT* c = ep->ContextRecord;
    if (g_logged++ >= kMaxLogged) return EXCEPTION_CONTINUE_SEARCH;

    // Which module faulted (game, a system DLL, the driver, or us).
    char modName[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(c->Rip), &mod)) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
    }
    const char* base = strrchr(modName, '\\');
    Log("CRASH? in %s+0x%llx", base ? base + 1 : modName,
        static_cast<unsigned long long>(c->Rip - reinterpret_cast<DWORD64>(mod)));

    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    Log("CRASH? exception 0x%08lx at rip=%p (%s address %p) thread %lu", code, reinterpret_cast<void*>(c->Rip),
        ep->ExceptionRecord->NumberParameters >= 2 ? (info[0] == 1 ? "writing" : info[0] == 8 ? "executing" : "reading")
                                                   : "?",
        ep->ExceptionRecord->NumberParameters >= 2 ? reinterpret_cast<void*>(info[1]) : nullptr, GetCurrentThreadId());
    Log("  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi);
    Log("  r8=%llx r9=%llx r10=%llx r11=%llx rsp=%llx rbp=%llx", c->R8, c->R9, c->R10, c->R11, c->Rsp, c->Rbp);

    CONTEXT ctx = *c;
    for (int frame = 0; frame < 20 && ctx.Rip; ++frame) {
        char frameMod[MAX_PATH] = "?";
        HMODULE fm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(ctx.Rip), &fm)) {
            GetModuleFileNameA(fm, frameMod, MAX_PATH);
        }
        const char* fmBase = strrchr(frameMod, '\\');
        Log("  #%d %p (%s+0x%llx)", frame, reinterpret_cast<void*>(ctx.Rip), fmBase ? fmBase + 1 : frameMod,
            static_cast<unsigned long long>(ctx.Rip - reinterpret_cast<DWORD64>(fm)));
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
    }
    // First-chance: the game may still handle it itself; we only log.
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLogger() {
    AddVectoredExceptionHandler(1, &CrashHandler);  // first in chain: the game's own crash handler would kill us first
    Log("crash logger installed");
}

}  // namespace rdmp
