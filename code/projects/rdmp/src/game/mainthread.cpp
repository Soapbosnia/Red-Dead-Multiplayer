// Main-thread task queue, driven by a detour on the script VM runner.
//
// FUN_1402066d0(int timeslice) runs all script threads; the game calls it every frame on the main thread.
// Prologue (12 position-independent bytes, then a RIP-relative load that we don't copy):
//   48 89 5c 24 08   mov [rsp+8],rbx
//   57               push rdi
//   48 83 ec 20      sub rsp,0x20
//   8b d9            mov ebx,ecx
// We overwrite those 12 bytes with "mov rax, FrameHook; jmp rax". The trampoline replays them and jumps back.
#include "mainthread.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kVmRunner = 0x1402066D0;
constexpr uint8_t kPrologue[12] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xD9};

using RunnerFn = void (*)(int timeslice);
RunnerFn g_trampoline = nullptr;

std::mutex g_queueMutex;
std::deque<std::function<void()>> g_queue;
std::vector<std::function<void(float)>> g_frameCallbacks;  // main thread only after registration

// Game clock in seconds (the AI code compares timers against it); advances once per frame.
constexpr uintptr_t kGameClock = 0x142314D3C;
float g_lastClock = -1.0f;

void FrameHook(int timeslice) {
    std::deque<std::function<void()>> work;
    std::vector<std::function<void(float)>> callbacks;
    {
        std::lock_guard lock(g_queueMutex);
        work.swap(g_queue);
        callbacks = g_frameCallbacks;
    }
    for (auto& fn : work) fn();
    const float clock = *reinterpret_cast<volatile float*>(GameAddr(kGameClock));
    if (clock != g_lastClock) {
        g_lastClock = clock;
        for (auto& fn : callbacks) fn(clock);
    }
    g_trampoline(timeslice);
}

}  // namespace

void RunOnMainThread(std::function<void()> fn) {
    std::lock_guard lock(g_queueMutex);
    g_queue.push_back(std::move(fn));
}

void AddFrameCallback(std::function<void(float)> fn) {
    std::lock_guard lock(g_queueMutex);
    g_frameCallbacks.push_back(std::move(fn));
}

void InstallMainThreadHook() {
    const uintptr_t at = GameAddr(kVmRunner);
    if (std::memcmp(reinterpret_cast<void*>(at), kPrologue, sizeof(kPrologue)) != 0) {
        Log("mainthread: VM runner prologue differs, not hooking");
        return;
    }
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!t) {
        Log("mainthread: VirtualAlloc failed");
        return;
    }
    // trampoline: original 12 bytes, then jmp [rip+0] -> at+12
    std::memcpy(t, kPrologue, 12);
    t[12] = 0xFF; t[13] = 0x25; std::memset(t + 14, 0, 4);
    const uint64_t back = at + 12;
    std::memcpy(t + 18, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), t, 26);
    g_trampoline = reinterpret_cast<RunnerFn>(t);

    if (ReplaceFunction(at, reinterpret_cast<const void*>(&FrameHook))) {
        Log("mainthread: per-frame hook on the VM runner installed (trampoline %p)", t);
    } else {
        Log("mainthread: failed to patch the VM runner (error %lu)", GetLastError());
    }
}

}  // namespace rdmp
