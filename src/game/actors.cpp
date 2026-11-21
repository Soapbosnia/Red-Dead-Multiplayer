// Actor creation.
//
// RE (re/NOTES.md, "Player creation"): a hardware write-watch on playerInfo+0x5ec during a normal boot
// showed the player is created by a script native, FUN_140232050(ctx), which reads its arguments from
// ctx[2] (4-byte slots) and calls FUN_1402211a0(out, a0, a1, a2, &pos, &rot, a7) -> actor handle.
// We replace the native with an identical re-implementation that also logs every call.
#include "actors.h"

#include <windows.h>

#include <cstring>

#include "log.h"
#include "memcheck.h"
#include "natives.h"
#include "patch.h"

namespace rdmp {

bool g_traceAfterPlayerCreate = false;

namespace {

constexpr uintptr_t kNativeCreateActor = 0x140232050;  // script native (replaced)
constexpr uintptr_t kCreateActor = 0x1402211A0;        // engine routine (called as-is)

using CreateActorFn = uint32_t* (*)(uint32_t* out, uint32_t a0, uint64_t a1, uint32_t a2, Vec3* pos, Vec3* rot,
                                   int32_t a7);

CreateActorFn RealCreateActor() { return reinterpret_cast<CreateActorFn>(GameAddr(kCreateActor)); }

// Native call context: [0] = result pointer, [2] = argument array (4-byte slots).
struct NativeCtx {
    void* result;
    uint64_t unk;
    uint8_t* args;
};

void SafeString(uint64_t p, char* out, size_t n) {
    out[0] = '\0';
    if (!IsReadable(p, n)) return;  // never fault: RDR's crash reporter fires on first-chance AVs
    const char* s = reinterpret_cast<const char*>(p);
    size_t i = 0;
    for (; i + 1 < n && s[i]; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7e) { out[0] = '\0'; return; }
        out[i] = s[i];
    }
    out[i] = '\0';
}

void NativeCreateActorHook(NativeCtx* ctx) {
    uint8_t* a = ctx->args;
    const uint32_t a0 = *reinterpret_cast<uint32_t*>(a + 0x00);
    const uint64_t a1 = *reinterpret_cast<uint64_t*>(a + 0x08);
    const uint32_t a2 = *reinterpret_cast<uint32_t*>(a + 0x10);
    Vec3 pos = *reinterpret_cast<Vec3*>(a + 0x18);
    Vec3 rot = *reinterpret_cast<Vec3*>(a + 0x28);
    const int32_t a7 = *reinterpret_cast<int32_t*>(a + 0x38);

    uint32_t out[8] = {};
    const uint32_t* r = RealCreateActor()(out, a0, a1, a2, &pos, &rot, a7);
    const uint32_t handle = r ? *r : 0;
    *reinterpret_cast<uint32_t*>(ctx->result) = handle;

    char name[64];
    SafeString(a1, name, sizeof(name));
    // Debug: see what the script does with the player right after creating it.
    if (g_traceAfterPlayerCreate && strcmp(name, "player") == 0) TraceNextNativeCalls(600);
    Log("native create actor: a0=%u (0x%x) a1=0x%llx \"%s\" a2=%u pos=(%.2f, %.2f, %.2f) rot=(%.2f, %.2f, %.2f) "
        "a7=%d -> handle 0x%08x",
        a0, a0, static_cast<unsigned long long>(a1), name, a2, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, a7,
        handle);
}

}  // namespace

void InstallActorHooks() {
    if (ReplaceFunction(GameAddr(kNativeCreateActor), reinterpret_cast<const void*>(&NativeCreateActorHook))) {
        Log("actors: create-actor native replaced (logging)");
    } else {
        Log("actors: failed to patch create-actor native (error %lu)", GetLastError());
    }
}

// Direct call to the actor factory FUN_1402209e0(out, owner, name, def, variation, &pos, &rot, 1), skipping the
// wrapper FUN_1402211a0, which refuses to create anything while playerInfo+0x5e9 is set (set once a player exists).
uint32_t CreateActorDirect(uint32_t owner, uint64_t name, uint32_t def, const Vec3& pos, const Vec3& rot, int32_t variation, uint8_t asPlayer) {
    using FactoryFn = uint32_t* (*)(uint32_t* out, uint32_t owner, uint64_t name, uint32_t def, int32_t variation,
                                    Vec3* pos, Vec3* rot, uint8_t asPlayer);
    uint32_t out[8] = {};
    Vec3 p = pos, r = rot;
    const uint32_t* res = reinterpret_cast<FactoryFn>(GameAddr(0x1402209E0))(out, owner, name, def, variation, &p, &r, asPlayer);
    return res ? *res : 0;
}

uint32_t CreateActor(uint32_t a0, uint64_t a1, uint32_t a2, const Vec3& pos, const Vec3& rot, int32_t a7) {
    uint32_t out[8] = {};
    Vec3 p = pos, r = rot;
    const uint32_t* res = RealCreateActor()(out, a0, a1, a2, &p, &r, a7);
    return res ? *res : 0;
}

}  // namespace rdmp
