// Locomotion "desire" override: makes remote-player puppets play the engine's own walk/run animations.
//
// RE (decomp/move_writer.c): the per-actor brain update (FUN_140b0ba10, worker threads) calls FUN_140b61910(self, state,
// ...), which calls the locomotion FUN_140ba6a80(loco, desire = state + 0x240, char, ptr, dt). The actor is
// *(loco + 0x80). desire is the per-frame movement request that the player's input (or an NPC's AI task) produced:
//   float[0x14] -> *(actor+0x38)+0xab4 (local move x), -float[0x16] -> +0xab0 (forward), [0x20..0x22] a world target, ...
// We detour FUN_140ba6a80: for the local player we copy the desire out; for puppets with an override we write our desire
// in before the original runs, so the engine animates them like a walking/running player.
#include "locomotion.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "log.h"
#include "memcheck.h"
#include "patch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kLocomotion = 0x140BA6A80;
// mov rax,rsp; mov [rax+20h],r9; mov [rax+18h],r8b; mov [rax+8],rcx  (15 bytes, position independent)
constexpr uint8_t kPrologue[15] = {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08};

using LocoFn = void (*)(uintptr_t loco, uint8_t* desire, uint64_t a3, uint64_t a4, uint64_t a5);
LocoFn g_original = nullptr;
std::atomic<uint32_t> g_applied{0};

std::atomic<uintptr_t> g_playerActor{0};

std::mutex g_mutex;
uint8_t g_playerDesire[kDesireSize];
bool g_havePlayerDesire = false;
uint32_t g_playerDesireSeq = 0;
// Debug autopilot: replaces the local player's own desire (tools: replaying a recorded route).
uint8_t g_playerOverride[kDesireSize];
bool g_havePlayerOverride = false;

// The local player's mount: its desire is what the rider's input asks of the horse.
std::atomic<uintptr_t> g_mountActor{0};
uint8_t g_mountDesire[kDesireSize];
bool g_haveMountDesire = false;
uint32_t g_mountDesireSeq = 0;

struct Override {
    uintptr_t actor = 0;
    uint8_t desire[kDesireSize];
};
Override g_overrides[32];

void LocoHook(uintptr_t loco, uint8_t* desire, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uintptr_t actor = loco ? *reinterpret_cast<uintptr_t*>(loco + 0x80) : 0;
    if (actor && desire) {
        if (actor == g_playerActor.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_mutex);
            if (g_havePlayerOverride) std::memcpy(desire, g_playerOverride, kDesireSize);
            std::memcpy(g_playerDesire, desire, kDesireSize);
            g_havePlayerDesire = true;
            ++g_playerDesireSeq;
        } else if (actor == g_mountActor.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_mutex);
            for (const Override& o : g_overrides) {  // a script-driven mount (tests) still reports what it was given
                if (o.actor == actor) {
                    std::memcpy(desire, o.desire, kDesireSize);
                    g_applied.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            std::memcpy(g_mountDesire, desire, kDesireSize);
            g_haveMountDesire = true;
            ++g_mountDesireSeq;
        } else {
            std::lock_guard lock(g_mutex);
            for (const Override& o : g_overrides) {
                if (o.actor == actor) {
                    std::memcpy(desire, o.desire, kDesireSize);
                    g_applied.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    g_original(loco, desire, a3, a4, a5);
}

}  // namespace

void InstallLocomotionHook() {
    const uintptr_t at = GameAddr(kLocomotion);
    if (std::memcmp(reinterpret_cast<void*>(at), kPrologue, sizeof(kPrologue)) != 0) {
        Log("loco: prologue differs, not hooking");
        return;
    }
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!t) return;
    std::memcpy(t, kPrologue, sizeof(kPrologue));
    t[15] = 0xFF; t[16] = 0x25; std::memset(t + 17, 0, 4);  // jmp [rip+0]
    const uint64_t back = at + sizeof(kPrologue);
    std::memcpy(t + 21, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), t, 29);
    g_original = reinterpret_cast<LocoFn>(t);
    if (ReplaceFunction(at, reinterpret_cast<const void*>(&LocoHook))) {
        Log("loco: locomotion hook installed (FUN_140ba6a80, trampoline %p)", t);
    } else {
        Log("loco: patch failed (%lu)", GetLastError());
    }
}

void SetLocoPlayerActor(uintptr_t actor) { g_playerActor = actor; }

bool GetPlayerDesire(uint8_t* out, uint32_t* seq) {
    std::lock_guard lock(g_mutex);
    if (!g_havePlayerDesire) return false;
    std::memcpy(out, g_playerDesire, kDesireSize);
    if (seq) *seq = g_playerDesireSeq;
    return true;
}

void SetPlayerDesireOverride(const uint8_t* desire) {
    std::lock_guard lock(g_mutex);
    g_havePlayerOverride = desire != nullptr;
    if (desire) std::memcpy(g_playerOverride, desire, kDesireSize);
}

void SetLocoMountActor(uintptr_t actor) {
    if (g_mountActor.exchange(actor) == actor) return;
    std::lock_guard lock(g_mutex);
    g_haveMountDesire = false;
}

bool GetMountDesire(uint8_t* out, uint32_t* seq) {
    std::lock_guard lock(g_mutex);
    if (!g_haveMountDesire) return false;
    std::memcpy(out, g_mountDesire, kDesireSize);
    if (seq) *seq = g_mountDesireSeq;
    return true;
}

void SetPuppetDesire(uintptr_t actor, const uint8_t* desire) {
    std::lock_guard lock(g_mutex);
    Override* freeSlot = nullptr;
    for (Override& o : g_overrides) {
        if (o.actor == actor) {
            std::memcpy(o.desire, desire, kDesireSize);
            return;
        }
        if (!o.actor && !freeSlot) freeSlot = &o;
    }
    if (freeSlot) {
        freeSlot->actor = actor;
        std::memcpy(freeSlot->desire, desire, kDesireSize);
    }
}

void ClearPuppetDesire(uintptr_t actor) {
    std::lock_guard lock(g_mutex);
    for (Override& o : g_overrides)
        if (o.actor == actor) o.actor = 0;
}

}  // namespace rdmp

namespace rdmp {
uint32_t LocoOverridesApplied() { return g_applied.load(); }
}  // namespace rdmp
