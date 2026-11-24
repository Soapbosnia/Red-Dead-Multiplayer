// Remote players driven by the game's own player code: see puppet_player.h.
#include "puppet_player.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>

#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "sandbox.h"

namespace rdmp {
namespace {

constexpr uintptr_t kPlayerInfoPtr = 0x1422DAF30;
constexpr size_t kPlayerInfoSize = 0x5f0;
constexpr size_t kFootControlSize = 0x1b90;
constexpr uintptr_t kControlUpdate = 0x1403726F0;     // FUN_1403726f0(ctrl): input records -> control state
constexpr uintptr_t kPlayerControlUpdate = 0x140BB4150;  // FUN_140bb4150(component, update): control -> movement
constexpr uintptr_t kComponentFactory = 0x140597660;  // FUN_140597660(&pool, descriptor) -> component
constexpr uintptr_t kComponentPool = 0x19A48D638;
constexpr uintptr_t kPlayerTemplate = 0x142AE0468;    // *(*(*(this)+0x18)+0x10): player template descriptors
constexpr uintptr_t kMarstonBehaveVt = 0x141D26BB8;
// mov rax,rsp; mov [rax+8],rbx; mov [rax+10h],rbp; mov [rax+18h],rsi  (15 bytes, position independent)
constexpr uint8_t kControlUpdatePrologue[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18};
// mov r11,rsp; push rbp; push rsi; push r13; push r14; push r15; lea rbp,[r11-0x5f]  (15 bytes, position independent)
constexpr uint8_t kPlayerControlPrologue[15] = {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x41, 0x55, 0x41,
                                                0x56, 0x41, 0x57, 0x49, 0x8D, 0x6B, 0xA1};

uintptr_t Ptr(uintptr_t addr) { return IsReadable(addr, 8) ? *reinterpret_cast<uintptr_t*>(addr) : 0; }
template <typename T>
T& At(uintptr_t base, size_t off) {
    return *reinterpret_cast<T*>(base + off);
}

// The actor's behaviour holder (actor+0x40, set up by FUN_140b09bd0) runs its active behaviour (holder+0x388) every
// frame (FUN_140b0c920). Its behaviour set (holder+0x370, 0x2050 bytes, FUN_140b6b9b0) embeds a MarstonBehave at
// +0x1950: the player's active behaviour, whose update is the player-control update FUN_140bb4150. NPCs have none.
uintptr_t MarstonBehaveOf(uintptr_t actor) {
    const uintptr_t holder = Ptr(actor + 0x40);
    const uintptr_t set = holder ? Ptr(holder + 0x370) : 0;
    const uintptr_t mb = set ? set + 0x1950 : 0;
    return mb && Ptr(mb) == GameAddr(kMarstonBehaveVt) ? mb : 0;
}
bool ActivateMarstonBehave(uintptr_t actor) {
    const uintptr_t holder = Ptr(actor + 0x40);
    const uintptr_t mb = MarstonBehaveOf(actor);
    if (!holder || !mb) return false;
    if (Ptr(holder + 0x388) != mb) At<uintptr_t>(holder, 0x388) = mb;
    return true;
}

struct Context {
    uint32_t goh = 0;
    uint32_t actorHandle = 0;
    uintptr_t actor = 0;
    alignas(16) uint8_t info[kPlayerInfoSize];        // this puppet's player-info
    alignas(16) uint8_t control[kFootControlSize];    // its on-foot control object
    alignas(16) uint8_t sub410[0x28], sub418[0x20], sub420[0x10], sub408[0x10];
    alignas(16) uint8_t records[32 * 16];             // its input records
    uintptr_t recordSet[2] = {};                      // what control+0x20 points at: { records, 0 }
    PlayerControls controls;
    uint8_t lastValue[kInputCount] = {};
    uint32_t heldMs[kInputCount] = {};
    ULONGLONG lastMs = 0;
    float aimTarget[3] = {}, aim[3] = {};  // the remote's aim point / the eased one written into the puppet
    float aimFrom[3] = {};                 // where the remote stands
    bool armed = false;                    // the remote has a weapon in hand (synced)
    bool camAiming = false;                // this frame: the remote aims; their camera = camEye -> camFwd
    float camEye[3] = {}, camFwd[3] = {};
    bool haveAim = false;
};

std::map<uint32_t, Context*> g_byGoh;
std::map<uint32_t, Context*> g_byActorHandle;
using PlayerControlFn = void (*)(uintptr_t component, void* update);
using ControlUpdateFn = void (*)(uintptr_t ctrl);
ControlUpdateFn g_controlUpdateOrig = nullptr;  // FUN_1403726f0 through the trampoline
bool g_localOverride = false;
bool g_localAimHeld = false;  // [debug] aim_hold: the local player's aim input (0x19) held
PlayerControls g_localControls;
PlayerControlFn g_playerControlOrig = nullptr;
std::atomic<int> g_switched{0};
constexpr uintptr_t kCameraManager = 0x142C45738;  // *(+8) = the gameplay camera: +0x1d0 right, +0x1e0 up, +0x1f0 back, +0x200 position
constexpr uintptr_t kReticleUpdate = 0x140348600;  // FUN_140348600(info): the reticle target point from the camera, and more
constexpr uintptr_t kInfoAimUpdate = 0x14034A6D0;  // FUN_14034a6d0(info): the player-info aim update (reads the reticle point)

// Runs fn with the gameplay camera looking from `eye` along `fwd` (unit) - the remote's view, as MTA swaps the camera
// rotation into the context of a remote player - and puts the real camera back right after. The camera exists twice:
// the frame camera *(manager+8) +0x1d0 (read by the control update, FUN_1403d9320) and the camera instance
// *(manager+0x28) +0x10 (read by the aim-matrix update FUN_140393550 and the camera code); both are rows right, up,
// back, position.
template <typename Fn>
void WithCamera(const float eye[3], const float fwd[3], Fn&& fn) {
    const uintptr_t mgr = Ptr(GameAddr(kCameraManager));
    const uintptr_t frame = mgr ? Ptr(mgr + 8) : 0, inst = mgr ? Ptr(mgr + 0x28) : 0;
    float* mats[2] = {frame && IsReadable(frame + 0x1d0, 0x40) ? reinterpret_cast<float*>(frame + 0x1d0) : nullptr,
                      inst && IsReadable(inst + 0x10, 0x40) ? reinterpret_cast<float*>(inst + 0x10) : nullptr};
    const float b[3] = {-fwd[0], -fwd[1], -fwd[2]};
    float r[3] = {b[2], 0.0f, -b[0]};  // up (0, 1, 0) x back
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-4f || (!mats[0] && !mats[1])) return fn();
    r[0] /= rl, r[2] /= rl;
    const float u[3] = {b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};  // back x right
    const float rows[4][3] = {{r[0], r[1], r[2]}, {u[0], u[1], u[2]}, {b[0], b[1], b[2]}, {eye[0], eye[1], eye[2]}};
    float saved[2][16];
    for (int k = 0; k < 2; ++k) {
        if (!mats[k]) continue;
        std::memcpy(saved[k], mats[k], sizeof(saved[k]));
        for (int i = 0; i < 4; ++i) mats[k][i * 4] = rows[i][0], mats[k][i * 4 + 1] = rows[i][1], mats[k][i * 4 + 2] = rows[i][2];
    }
    fn();
    for (int k = 0; k < 2; ++k)
        if (mats[k]) std::memcpy(mats[k], saved[k], sizeof(saved[k]));
}


// The puppet's input records from its controller state, the way the game's mapper keeps them: [2] new value,
// [3] the previous one, [1] a timer (frames held, in the game's own ~4/frame steps).
void FillRecords(Context& c, ULONGLONG now) {
    const uint32_t dt = c.lastMs ? static_cast<uint32_t>(now - c.lastMs) : 16;
    c.lastMs = now;
    for (int id = 0; id < kInputCount; ++id) {
        uint8_t* r = c.records + id * 16;
        const bool analog = id == 0x11 || id == 0x12;
        const uint8_t v = c.controls.valid ? c.controls.value[id] : static_cast<uint8_t>(analog ? 0x7f : 0);
        r[0] = 0;  // no invert: the value already is the effective one
        r[3] = r[2];
        r[2] = v;
        if (v != c.lastValue[id]) c.heldMs[id] = 0;
        else c.heldMs[id] += dt;
        c.lastValue[id] = v;
        r[1] = static_cast<uint8_t>(c.heldMs[id] / 4 > 255 ? 255 : c.heldMs[id] / 4);
    }
}

// Copies the local player's player-info / control object into the context and points them at the puppet.
bool BuildContext(Context& c) {
    const uintptr_t info = Ptr(GameAddr(kPlayerInfoPtr));
    const uintptr_t ctrl = info ? Ptr(info + 0x118) : 0;
    if (!info || !ctrl || !IsReadable(ctrl, kFootControlSize)) return false;
    std::memcpy(c.info, reinterpret_cast<void*>(info), kPlayerInfoSize);
    std::memcpy(c.control, reinterpret_cast<void*>(ctrl), kFootControlSize);
    const uintptr_t pi = reinterpret_cast<uintptr_t>(c.info), pc = reinterpret_cast<uintptr_t>(c.control);
    // private copies of the small per-player objects the on-foot code writes
    auto clone = [&](size_t off, uint8_t* buf, size_t size) {
        const uintptr_t src = Ptr(info + off);
        if (src && IsReadable(src, size)) std::memcpy(buf, reinterpret_cast<void*>(src), size);
        At<uintptr_t>(pi, off) = reinterpret_cast<uintptr_t>(buf);
    };
    clone(0x410, c.sub410, sizeof(c.sub410));
    clone(0x418, c.sub418, sizeof(c.sub418));
    clone(0x420, c.sub420, sizeof(c.sub420));
    clone(0x408, c.sub408, sizeof(c.sub408));
    At<uintptr_t>(reinterpret_cast<uintptr_t>(c.sub410), 0) = pi;  // +0x410 points back at its player-info
    At<uintptr_t>(pi, 0x118) = pc;
    At<uint32_t>(pi, 0x5ec) = c.actorHandle;
    At<uint8_t>(pi, 0x5e9) = 1;
    At<uint8_t>(pi, 0x1c8) = 1;
    At<uint32_t>(pi, 0x5d4) = 2;  // on foot
    At<uint32_t>(pi, 0x5d8) = 2;
    At<uintptr_t>(pc, 0x40) = pi;  // owner
    c.recordSet[0] = reinterpret_cast<uintptr_t>(c.records);
    At<uintptr_t>(pc, 0x20) = reinterpret_cast<uintptr_t>(c.recordSet);
    At<uintptr_t>(pc, 0x1c8) = 0;  // a reference-counted pointer of the local player's: not ours to release
    std::memset(c.records, 0, sizeof(c.records));
    FillRecords(c, GetTickCount64());
    return true;
}

// The player-control components the game creates for the player actor (FUN_140ae3ff0, asPlayer): +0xb8 and +0xd8.
bool AddPlayerComponents(uintptr_t actor) {
    const uintptr_t tmpl = Ptr(Ptr(Ptr(GameAddr(kPlayerTemplate)) + 0x18) + 0x10);
    if (!tmpl) return false;
    using Factory = uintptr_t* (*)(uintptr_t pool, uintptr_t descriptor);
    for (const size_t slot : {size_t(0xb8), size_t(0xd8)}) {
        if (Ptr(actor + slot)) continue;
        const uintptr_t desc = Ptr(tmpl + (slot == 0xb8 ? 0xb8 : 0xe8));
        if (!desc) return false;
        uintptr_t* comp = reinterpret_cast<Factory>(GameAddr(kComponentFactory))(GameAddr(kComponentPool), desc);
        if (!comp) return false;
        At<uintptr_t*>(actor, 0x18 + static_cast<size_t>(At<int>(desc, 0x10)) * 8) = comp;
        comp[2] = actor;
        comp[1] = desc;
        using VFn = void (*)(uintptr_t*);
        reinterpret_cast<VFn>(Ptr(comp[0] + 8))(comp);
        reinterpret_cast<VFn>(Ptr(comp[0] + 0x18))(comp);
        Log("puppet player: actor %p component +0x%zx = %p (descriptor %p)", reinterpret_cast<void*>(actor), slot,
            reinterpret_cast<void*>(comp), reinterpret_cast<void*>(desc));
    }
    return true;
}

// Runs fn with the global playerInfo pointing at the puppet's (MTA's SwitchContext / ReturnContextToLocalPlayer).
template <typename Fn>
void InContext(Context& c, Fn&& fn) {
    uintptr_t* global = reinterpret_cast<uintptr_t*>(GameAddr(kPlayerInfoPtr));
    const uintptr_t local = *global;
    *global = reinterpret_cast<uintptr_t>(c.info);
    g_switched.fetch_add(1, std::memory_order_relaxed);
    fn();
    *global = local;
}

void PlayerControlHook(uintptr_t component, void* update) {
    // component +0x60 -> +0x10 = its actor entry (+8 = actor handle)
    const uintptr_t a60 = Ptr(component + 0x60), a10 = a60 ? Ptr(a60 + 0x10) : 0;
    const uint32_t handle = a10 && IsReadable(a10 + 8, 4) ? At<uint32_t>(a10, 8) : 0;
    const auto it = handle ? g_byActorHandle.find(handle) : g_byActorHandle.end();
    if (it == g_byActorHandle.end()) {
        g_playerControlOrig(component, update);
        return;
    }
    Context& c = *it->second;
    auto run = [&] { InContext(c, [&] { g_playerControlOrig(component, update); }); };
    if (c.camAiming) WithCamera(c.camEye, c.camFwd, run);
    else run();
}

// FUN_1403726f0 for the local player's control object, with the autopilot's input written into its records first.
void ControlUpdateHook(uintptr_t ctrl) {
    const uintptr_t info = Ptr(GameAddr(kPlayerInfoPtr));
    if (g_localAimHeld && info && Ptr(info + 0x118) == ctrl) {
        const uintptr_t set = Ptr(ctrl + 0x20), recs = set ? Ptr(set) : 0;
        if (recs && IsReadable(recs + 0x19 * 16, 16)) {
            uint8_t* r = reinterpret_cast<uint8_t*>(recs + 0x19 * 16);
            r[3] = r[2];
            r[2] = static_cast<uint8_t>(0xff ^ r[0]);
        }
    }
    const bool local = g_localOverride && info && Ptr(info + 0x118) == ctrl;
    if (local) {
        const uintptr_t set = Ptr(ctrl + 0x20), recs = set ? Ptr(set) : 0;
        if (recs && IsReadable(recs, kInputCount * 16)) {
            for (int id = 0; id < kInputCount; ++id) {
                uint8_t* r = reinterpret_cast<uint8_t*>(recs + id * 16);
                r[3] = r[2];
                r[2] = static_cast<uint8_t>(g_localControls.value[id] ^ r[0]);
            }
        }
    }
    g_controlUpdateOrig(ctrl);
    if (local && g_localControls.heading != 0.0f) {
        At<float>(ctrl, 0x104) = g_localControls.heading;
        At<float>(ctrl, 0x100) = g_localControls.heading;
    }
}

}  // namespace

void SetLocalControlsOverride(const PlayerControls* controls) {
    g_localOverride = controls != nullptr;
    if (controls) g_localControls = *controls;
}

void InstallPuppetPlayerHooks() {
    g_playerControlOrig = reinterpret_cast<PlayerControlFn>(DetourPic(
        GameAddr(kPlayerControlUpdate), kPlayerControlPrologue, sizeof(kPlayerControlPrologue), reinterpret_cast<void*>(&PlayerControlHook)));
    Log("puppet player: player-control hook %s", g_playerControlOrig ? "installed" : "FAILED (prologue differs)");
    g_controlUpdateOrig = reinterpret_cast<ControlUpdateFn>(DetourPic(
        GameAddr(kControlUpdate), kControlUpdatePrologue, sizeof(kControlUpdatePrologue), reinterpret_cast<void*>(&ControlUpdateHook)));
    Log("puppet player: control-update hook %s", g_controlUpdateOrig ? "installed" : "FAILED (prologue differs)");
}

bool AttachPlayerControl(uint32_t goh) {
    if (!g_playerControlOrig) return false;
    if (g_byGoh.count(goh)) return true;
    const uintptr_t actor = ActorFromGohHandle(goh);
    const uint32_t handle = ActorHandleFromGohHandle(goh);
    if (!actor || !handle) return false;
    auto* c = new Context();
    c->goh = goh;
    c->actor = actor;
    c->actorHandle = handle;
    if (!BuildContext(*c) || !AddPlayerComponents(actor) || !ActivateMarstonBehave(actor)) {
        Log("puppet player: goh 0x%08x: could not set up player control", goh);
        delete c;
        return false;
    }
    g_byGoh[goh] = c;
    g_byActorHandle[handle] = c;
    Log("puppet player: goh 0x%08x (actor %p, handle 0x%08x) driven by player code", goh, reinterpret_cast<void*>(actor), handle);
    return true;
}

void DetachPlayerControl(uint32_t goh) {
    const auto it = g_byGoh.find(goh);
    if (it == g_byGoh.end()) return;
    g_byActorHandle.erase(it->second->actorHandle);
    delete it->second;
    g_byGoh.erase(it);
}

bool HasPlayerControl(uint32_t goh) { return g_byGoh.count(goh) != 0; }

void SetLocalAimHeld(bool held) { g_localAimHeld = held; }

void SetPlayerArmed(uint32_t goh, bool armed) {
    const auto it = g_byGoh.find(goh);
    if (it != g_byGoh.end()) it->second->armed = armed;
}

void SetPlayerAim(uint32_t goh, float x, float y, float z, float fromX, float fromY, float fromZ) {
    const auto it = g_byGoh.find(goh);
    if (it == g_byGoh.end()) return;
    Context& c = *it->second;
    c.aimTarget[0] = x, c.aimTarget[1] = y, c.aimTarget[2] = z;
    c.aimFrom[0] = fromX, c.aimFrom[1] = fromY, c.aimFrom[2] = fromZ;
    c.haveAim = x != 0.0f || y != 0.0f || z != 0.0f;
}


void SetPlayerControls(uint32_t goh, const PlayerControls& controls) {
    const auto it = g_byGoh.find(goh);
    if (it != g_byGoh.end()) it->second->controls = controls;
}

bool CaptureLocalControls(PlayerControls& out) {
    const uintptr_t info = Ptr(GameAddr(kPlayerInfoPtr));
    const uintptr_t ctrl = info ? Ptr(info + 0x118) : 0;
    const uintptr_t set = ctrl ? Ptr(ctrl + 0x20) : 0;
    const uintptr_t recs = set ? Ptr(set) : 0;
    if (!recs || !IsReadable(recs, kInputCount * 16)) return false;
    for (int id = 0; id < kInputCount; ++id) {
        const uint8_t* r = reinterpret_cast<const uint8_t*>(recs + id * 16);
        out.value[id] = r[2] ^ r[0];
    }
    out.heading = At<float>(ctrl, 0x104);
    out.valid = true;
    return true;
}

void PuppetPlayerFrame() {
    const ULONGLONG now = GetTickCount64();
    for (auto& [goh, c] : g_byGoh) {
        // the actor instance changes on stream out / in: follow it (the context is rebuilt by the caller then)
        if (ActorFromGohHandle(goh) != c->actor) continue;
        ActivateMarstonBehave(c->actor);
        FillRecords(*c, now);
        const uintptr_t ctrl = reinterpret_cast<uintptr_t>(c->control);
        // The local player's frame (FUN_14034c830 -> FUN_14034f700), for the puppet's control object: the per-frame
        // resets, the input step (FUN_1403726f0: stick, magnitude, desired heading, sprint state), then the control
        // object's vfunc +0x28 (FUN_140372e50: the move vector, gait and turn the movement request is built from).
        At<uint32_t>(ctrl, 0x160) = 0;
        At<uint32_t>(ctrl, 0x18c) = 0;
        At<uint64_t>(ctrl, 0x104) = 0;
        At<uint64_t>(ctrl, 0x144) = 0;
        At<uint32_t>(ctrl, 0x100) = 0;
        At<uint64_t>(ctrl, 0x124) = 0;
        At<uint32_t>(ctrl, 0x120) = 0;
        At<uint32_t>(ctrl, 0x1a0) = 0;
        // MTA's context switch includes the remote's camera: while the remote aims, the puppet's update runs with the
        // gameplay camera looking from the remote's eye to their aim point, so the game's own player code turns it into
        // the aim (aim mode, the arm's yaw and pitch, the reticle) exactly as it does for the local player.
        bool aiming = c->controls.valid && c->controls.value[0x19] > 0x7f && c->haveAim && c->armed;
        const float eye[3] = {c->aimFrom[0], c->aimFrom[1] + 1.6f, c->aimFrom[2]};
        float fwd[3] = {c->aimTarget[0] - eye[0], c->aimTarget[1] - eye[1], c->aimTarget[2] - eye[2]};
        const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl < 0.5f) aiming = false;
        else for (float& v : fwd) v /= fl;
        c->camAiming = aiming;  // the actor's player-control update (PlayerControlHook) gets the same camera
        std::memcpy(c->camEye, eye, sizeof(eye));
        std::memcpy(c->camFwd, fwd, sizeof(fwd));
        const uintptr_t pi = reinterpret_cast<uintptr_t>(c->info);
        // The remote's aim mode. RDR does not enter it from the aim button in the player code: the camera system switches
        // to its aim camera (state of the local player's camera, which a puppet has none of) and the player follows. The
        // game's own switch for a player's aim mode (playerInfo +0x3bc bit 0x02, what scripts set with
        // SET_FORCE_PLAYER_AIM_MODE) mirrors the remote's state: on exactly while they aim.
        uint8_t& aimMode = At<uint8_t>(pi, 0x3bc);
        aimMode = aiming ? (aimMode | 2) : (aimMode & ~2);
        auto update = [&] {
            InContext(*c, [&] {
                using Fn = void (*)(uintptr_t);
                if (g_controlUpdateOrig) g_controlUpdateOrig(ctrl);
                else reinterpret_cast<Fn>(GameAddr(kControlUpdate))(ctrl);
                // Not aiming the puppet has no camera of the remote's, and the stick is camera-relative: its desired
                // heading is the remote's own (captured after the remote's camera turned it into a world heading).
                if (c->controls.valid && !aiming) {
                    At<float>(ctrl, 0x104) = c->controls.heading;
                    At<float>(ctrl, 0x100) = c->controls.heading;
                } else if (aiming && At<float>(ctrl, 0x104) == 0.0f) {
                    At<float>(ctrl, 0x104) = At<float>(ctrl, 0x100);  // aiming, standing: the body follows the camera
                }
                reinterpret_cast<Fn>(Ptr(Ptr(ctrl) + 0x28))(ctrl);
                if (aiming) {  // the local player's aim steps (game loop 0x1401F8344 .. 0x1401F83CA)
                    reinterpret_cast<Fn>(GameAddr(kReticleUpdate))(pi);
                    reinterpret_cast<Fn>(GameAddr(kInfoAimUpdate))(pi);
                }
            });
        };
        if (aiming) WithCamera(eye, fwd, update);
        else update();
        // The remote's aim point (already interpolated on the remote's timeline): the puppet's reticle target point.
        const uintptr_t comp = Ptr(c->actor + 0xb8);
        if (c->haveAim && comp && IsReadable(comp + 0x3800, 0x30)) {
            std::memcpy(c->aim, c->aimTarget, sizeof(c->aim));
            for (const size_t off : {size_t(0x3800), size_t(0x3810), size_t(0x3820)}) std::memcpy(reinterpret_cast<void*>(comp + off), c->aim, sizeof(c->aim));
        }
    }
    static ULONGLONG nextLog = 0;
    if (!g_byGoh.empty() && now > nextLog) {
        nextLog = now + 10000;
        Log("puppet player: %zu puppet(s) under player control, %d context switches", g_byGoh.size(), g_switched.load());
        for (auto& [goh, c] : g_byGoh)
            Log("puppet player: goh 0x%08x aim input %u armed %d, aim point (%.1f, %.1f, %.1f), camera forward (%.2f, %.2f, %.2f), "
                "aim mode %02x",
                goh, c->controls.value[0x19], c->armed ? 1 : 0, c->aim[0], c->aim[1], c->aim[2],
                At<float>(reinterpret_cast<uintptr_t>(c->info), 0x30), At<float>(reinterpret_cast<uintptr_t>(c->info), 0x34),
                At<float>(reinterpret_cast<uintptr_t>(c->info), 0x38), At<uint8_t>(reinterpret_cast<uintptr_t>(c->info), 0x3bc));
    }
}

}  // namespace rdmp
