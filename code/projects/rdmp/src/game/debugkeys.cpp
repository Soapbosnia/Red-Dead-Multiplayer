// Debug hotkeys for boot/sandbox experiments. RE notes: re/NOTES.md ("SP sandbox").
#include "debugkeys.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "memcheck.h"
#include "mainthread.h"
#include "patch.h"
#include "sandbox.h"

namespace rdmp {
namespace {

// Loading-screen controller (*(void**)0x142c456c8). The game's frame update (FUN_1401f3ad0) calls
// FinishLoading (FUN_14063b0a0) when bit 0x10 of byte +0x257e5 is set; args come from +0x25814/+0x25818.
constexpr uintptr_t kLoaderPtr = 0x142C456C8;
constexpr uintptr_t kLoaderFlags = 0x257E5;
constexpr uintptr_t kLoaderArgA = 0x25814;
constexpr uintptr_t kLoaderArgB = 0x25818;

// Local player info (*(void**)0x1422daf30); +0x5ec = actor handle (lo16 index, hi16 generation).
// Handle table (*(void**)0x142c457f8): 16-byte entries { object*, u16 generation }.
constexpr uintptr_t kPlayerInfoPtr = 0x1422DAF30;
constexpr uintptr_t kPlayerActorHandle = 0x5EC;
constexpr uintptr_t kHandleTablePtr = 0x142C457F8;

template <typename T>
T Read(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }

void RequestFinishLoading() {
    const uintptr_t loader = Read<uintptr_t>(GameAddr(kLoaderPtr));
    if (!loader) { Log("F5: loader object is null"); return; }
    auto* flags = reinterpret_cast<uint8_t*>(loader + kLoaderFlags);
    Log("F5: loader=%p flags=0x%02x argA=%u argB=%u -> setting finish-loading bit", reinterpret_cast<void*>(loader),
        *flags, Read<uint32_t>(loader + kLoaderArgA), Read<uint32_t>(loader + kLoaderArgB));
    *flags = static_cast<uint8_t>(*flags | 0x10);
}

// Spawn position used by the sandbox (rdmp.ini); a world position near it marks a position field.
constexpr float kRefX = 736.49f, kRefY = 89.17f, kRefZ = 1199.33f, kTol = 60.0f;

bool NearRef(const float* f) {
    return std::fabs(f[0] - kRefX) < kTol && std::fabs(f[1] - kRefY) < kTol && std::fabs(f[2] - kRefZ) < kTol;
}

// Scans the actor object and every object it points to (one level) for a vec3 near the spawn position.
// Logs matches as "actor+off" or "actor+ptrOff -> +off", with the values, so we learn where position lives.
void FindPositionFields(uintptr_t actor) {
    int hits = 0;
    if (!IsReadable(actor, 0x800)) return;
    for (uintptr_t off = 0; off + 12 <= 0x800 && hits < 16; off += 4) {
        const float* f = reinterpret_cast<const float*>(actor + off);
        if (NearRef(f)) {
            Log("F7:   pos? actor+0x%llx = (%.2f, %.2f, %.2f)", static_cast<unsigned long long>(off), f[0], f[1], f[2]);
            ++hits;
        }
    }
    for (uintptr_t p = 0; p + 8 <= 0x400 && hits < 16; p += 8) {
        const uintptr_t sub = *reinterpret_cast<uintptr_t*>(actor + p);
        if (sub < 0x10000 || (sub & 7) || !IsReadable(sub, 0x400)) continue;
        for (uintptr_t off = 0; off + 12 <= 0x400 && hits < 16; off += 4) {
            const float* f = reinterpret_cast<const float*>(sub + off);
            if (NearRef(f)) {
                Log("F7:   pos? [actor+0x%llx]=%p +0x%llx = (%.2f, %.2f, %.2f)", static_cast<unsigned long long>(p),
                    reinterpret_cast<void*>(sub), static_cast<unsigned long long>(off), f[0], f[1], f[2]);
                ++hits;
            }
        }
    }
    if (!hits) Log("F7:   no field near the spawn position found");
}

// F8 snapshot/diff: first press copies the player actor (0x800) and every object it points to (0x400 each);
// second press logs vec3s that changed and look like world positions near the spawn (to find the live position).
struct Snap {
    uintptr_t base = 0;
    uint8_t data[0x800];
};
Snap g_snapActor;
Snap g_snapSub[0x80];
int g_snapState = 0;

uintptr_t PlayerActor() {
    const uintptr_t info = Read<uintptr_t>(GameAddr(kPlayerInfoPtr));
    if (!IsReadable(info + kPlayerActorHandle, 4)) return 0;
    const uint32_t handle = Read<uint32_t>(info + kPlayerActorHandle);
    const uintptr_t table = Read<uintptr_t>(GameAddr(kHandleTablePtr));
    const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
    if (!IsReadable(entry, 0x10) || Read<uint16_t>(entry + 8) != static_cast<uint16_t>(handle >> 16)) return 0;
    return Read<uintptr_t>(entry);
}

void DiffSnap(const Snap& s, size_t len, const char* label) {
    if (!s.base || !IsReadable(s.base, len)) return;
    for (size_t off = 0; off + 12 <= len; off += 4) {
        const float* a = reinterpret_cast<const float*>(s.data + off);
        const float* b = reinterpret_cast<const float*>(s.base + off);
        if (!NearRef(b)) continue;  // must still look like a position near the spawn
        const float d = std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]);
        if (d > 0.2f) {
            Log("F8:   %s+0x%llx: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)", label, static_cast<unsigned long long>(off),
                a[0], a[1], a[2], b[0], b[1], b[2]);
        }
    }
}

// F8 (3-phase): snapshot at idle, while moving (hold W), idle again; logs floats that are ~0 at both idles and clearly
// non-zero while moving -> the player's movement intent / locomotion fields (actor + every object it points to).
struct BigSnap {
    uintptr_t base = 0;
    size_t len = 0;
    uint8_t data[0x1000];
};
BigSnap g_mv[3][0x81];  // [phase][0 = actor, 1.. = actor+8*k pointee]
int g_mvPhase = 0;

void TakeMoveSnapshot(int phase, uintptr_t actor) {
    auto take = [](BigSnap& s, uintptr_t base, size_t len) {
        s.base = 0;
        if (base < 0x10000 || (base & 7) || !IsReadable(base, len)) return;
        s.base = base;
        s.len = len;
        std::memcpy(s.data, reinterpret_cast<void*>(base), len);
    };
    take(g_mv[phase][0], actor, 0x800);
    for (int k = 1; k <= 0x80; ++k) take(g_mv[phase][k], Read<uintptr_t>(actor + (k - 1) * 8), 0x1000);
}

void MoveDiff() {
    int hits = 0;
    for (int k = 0; k <= 0x80 && hits < 120; ++k) {
        const BigSnap& a = g_mv[0][k];
        const BigSnap& b = g_mv[1][k];
        const BigSnap& c = g_mv[2][k];
        if (!a.base || a.base != b.base || a.base != c.base) continue;
        for (size_t off = 0; off + 4 <= a.len && hits < 120; off += 4) {
            float fa, fb, fc;
            std::memcpy(&fa, a.data + off, 4);
            std::memcpy(&fb, b.data + off, 4);
            std::memcpy(&fc, c.data + off, 4);
            if (!(std::fabs(fa) < 0.05f && std::fabs(fc) < 0.05f && std::fabs(fb) > 0.3f && std::fabs(fb) < 50.0f)) continue;
            Log("F8 move:   %s+0x%x+0x%llx: idle %.3f -> moving %.3f -> idle %.3f",
                k == 0 ? "actor" : "[actor", k == 0 ? 0 : (k - 1) * 8, static_cast<unsigned long long>(off), fa, fb, fc);
            ++hits;
        }
    }
    Log("F8 move: %d candidate field(s)", hits);
}

void SnapshotOrDiff() {
    {
        const uintptr_t actor = PlayerActor();
        if (!actor) { Log("F8: no player actor"); return; }
        TakeMoveSnapshot(g_mvPhase, actor);
        Log("F8 move: phase %d snapshot (actor %p)", g_mvPhase, reinterpret_cast<void*>(actor));
        if (++g_mvPhase == 3) {
            g_mvPhase = 0;
            MoveDiff();
        }
        return;
    }
    const uintptr_t actor = PlayerActor();
    if (!actor || !IsReadable(actor, 0x800)) { Log("F8: no player actor"); return; }
    if (g_snapState == 0) {
        g_snapActor.base = actor;
        std::memcpy(g_snapActor.data, reinterpret_cast<void*>(actor), 0x800);
        int n = 0;
        for (uintptr_t p = 0; p < 0x400 && n < 0x80; p += 8) {
            const uintptr_t sub = Read<uintptr_t>(actor + p);
            Snap& s = g_snapSub[p / 8];
            s.base = 0;
            if (sub < 0x10000 || (sub & 7) || !IsReadable(sub, 0x400)) continue;
            s.base = sub;
            std::memcpy(s.data, reinterpret_cast<void*>(sub), 0x400);
            ++n;
        }
        g_snapState = 1;
        Log("F8: snapshot taken (actor %p + %d sub-objects); move, then press F8 again", reinterpret_cast<void*>(actor), n);
    } else {
        Log("F8: diff vs snapshot:");
        DiffSnap(g_snapActor, 0x800, "actor");
        for (int i = 0; i < 0x80; ++i) {
            if (!g_snapSub[i].base) continue;
            char label[32];
            sprintf_s(label, "[actor+0x%x]", i * 8);
            DiffSnap(g_snapSub[i], 0x400, label);
        }
        g_snapState = 0;
        Log("F8: diff done");
    }
}

// Dumps the camera manager (*(0x142c45738)): each qword, and for pointers the pointee's vtable
// (map vtables to class names offline with ghidra_out/RDR.exe/vtables.tsv + getname_list.c).
void DumpCameraManager() {
    constexpr uintptr_t kCameraMgrPtr = 0x142C45738;
    const uintptr_t mgr = Read<uintptr_t>(GameAddr(kCameraMgrPtr));
    if (!IsReadable(mgr, 0x300)) { Log("F7: camera manager not readable"); return; }
    const uintptr_t lo = GameBase(), hi = GameBase() + 0x5A5EC600;
    Log("F7: camMgr=%p vt=%p", reinterpret_cast<void*>(mgr), reinterpret_cast<void*>(Read<uintptr_t>(mgr)));
    for (uintptr_t off = 8; off < 0x300; off += 8) {
        const uintptr_t v = Read<uintptr_t>(mgr + off);
        if (!v) continue;
        uintptr_t vt = 0;
        if (v >= 0x10000 && !(v & 7) && IsReadable(v, 8)) {
            vt = Read<uintptr_t>(v);
            if (vt < lo || vt >= hi) vt = 0;  // pointee isn't an object with a game vtable
        }
        if (vt) Log("F7:   camMgr+0x%03llx = %p -> obj vt %p", static_cast<unsigned long long>(off), reinterpret_cast<void*>(v), reinterpret_cast<void*>(vt));
        else if (v < 0x100000000ULL) Log("F7:   camMgr+0x%03llx = 0x%llx", static_cast<unsigned long long>(off), static_cast<unsigned long long>(v));
    }
}

void LogPlayer() {
    const uintptr_t info = Read<uintptr_t>(GameAddr(kPlayerInfoPtr));
    if (!IsReadable(info + kPlayerActorHandle, 4)) { Log("F7: player info not readable (%p)", reinterpret_cast<void*>(info)); return; }
    const uint32_t handle = Read<uint32_t>(info + kPlayerActorHandle);
    const uintptr_t table = Read<uintptr_t>(GameAddr(kHandleTablePtr));
    uintptr_t actor = 0;
    if (handle != 0xFFFFFFFF && IsReadable(table + (handle & 0xFFFF) * 0x10, 0x10)) {
        const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
        if (Read<uint16_t>(entry + 8) == static_cast<uint16_t>(handle >> 16)) actor = Read<uintptr_t>(entry);
    }
    Log("F7: playerInfo=%p actorHandle=0x%08x handleTable=%p actor=%p", reinterpret_cast<void*>(info), handle,
        reinterpret_cast<void*>(table), reinterpret_cast<void*>(actor));
    if (actor) FindPositionFields(actor);
    DumpCameraManager();
}

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI KeyThread(LPVOID) {
    bool prev5 = false, prev6 = false, prev7 = false, prev8 = false;
    for (;;) {
        Sleep(50);
        const bool focused = GameFocused();
        const bool f5 = focused && (GetAsyncKeyState(VK_F5) & 0x8000);
        const bool f6 = focused && (GetAsyncKeyState(VK_F6) & 0x8000);
        const bool f7 = focused && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (f5 && !prev5) RequestFinishLoading();
        if (f6 && !prev6) CameraToPlayer("F6");
        if (f7 && !prev7) LogPlayer();
        const bool f8 = focused && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (f8 && !prev8) SnapshotOrDiff();
        prev8 = f8;
        static bool prev9 = false;
        const bool f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (f9 && !prev9) {
            Log("F9: queueing puppet spawn on the main thread");
            RunOnMainThread([] { SpawnPuppetNearPlayer(); });
        }
        prev9 = f9;
        static bool prev10 = false;
        const bool f10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
        if (f10 && !prev10) RunOnMainThread([] { TogglePuppetOrbit(); });
        prev10 = f10;
        static bool prev11 = false;
        const bool f11 = focused && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (f11 && !prev11) RunOnMainThread([] { PuppetsComeToPlayer(); });
        prev11 = f11;
        prev5 = f5;
        prev6 = f6;
        prev7 = f7;
    }
}

}  // namespace

void StartDebugKeys() {
    HANDLE t = CreateThread(nullptr, 0, &KeyThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    Log("debug keys: F5 = finish loading screen, F7 = log player state");
}

}  // namespace rdmp
