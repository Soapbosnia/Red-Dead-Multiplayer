// Sandbox boot: world + player only (no story scripts).
//
// RE (re/NOTES.md "SP sandbox boot findings" / "Player creation"):
// - root script sp_idle loads the world but never ends the loading screen and never creates the player.
// - Loading ends when bit 0x10 of loader+0x257e5 is set (loader = *(0x142c456c8)); the frame update
//   FUN_1401f3ad0 then calls FinishLoading once the world is ready, which posts "loadComplete".
// - The story script creates the player with the create-actor native:
//     a0=808, a1="player", a2=0, pos=(736.49, 89.17, 1199.33), rot=0, a7=12
#include "sandbox.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "actors.h"
#include "events.h"
#include "log.h"
#include "mainthread.h"
#include "memcheck.h"
#include "patch.h"
#include "scriptnatives.h"
#include "locomotion.h"
#include "bootvideo.h"
#include "gameflow.h"
#include "splash.h"
#include "watch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kLoaderPtr = 0x142C456C8;
constexpr uintptr_t kLoaderFlags = 0x257E5;

bool g_enabled = false;
bool g_autoFinish = true;
bool g_spawnPlayer = false; // script-driven: nothing spawns at boot (rdmp.ini [sandbox] spawn_player)
uint32_t g_playerActorType = 808;
Vec3 g_spawnPos{736.49f, 89.17f, 1199.33f};
float g_spawnHeading = 0.0f;

bool g_inSinglePlayer = false;   // set after "enterSinglePlayer" (so the boot-time loadComplete is ignored)
bool g_worldLoading = false;     // set when the in-game world load has started (root script requested)
ULONGLONG g_worldLoadStartMs = 0;
uint32_t g_finishDelayMs = 8000;
bool g_engineFade = false;     // ini engine_fade: black via the loader fader while loading, engine fade-in at the end
float g_fadeInSec = 1.5f;      // ini fade_in_ms
uint32_t g_fadeHoldMs = 1000;  // ini fade_hold_ms: black kept this long after the player spawns (camera settles)
ULONGLONG g_fadeInAt = 0;      // tick when the held fade-in gets its real speed
// Owner object the story script spawns the player into: goh[808], a static object at 0x1422D9F50 (handle stable
// across runs). Spawning into an ordinary Layout creates the actor but doesn't make it the local player.
uint32_t g_ownerHandle = 808;
bool g_skipMenus = true;
int g_skipMenusMode = 1;  // 1 = press through the menus, 2 = direct start (no legal screen / title / menus)
char g_directStartEvent[64] = "fileDoFreeSpaceCheck";  // skip_menus=2 trigger (rdmp.ini direct_start_event)
bool g_killScripts = true;
bool g_clearScriptCamera = true;
uint32_t g_puppetDef = 0;  // actor def index for remote-player puppets (0 = template_player = Marston; safe with asPlayer=0)
uint32_t g_puppetsAtSpawn = 0;
// 0 = first base Layout. NOT 808: an actor in the player's owner object becomes a second "player" -> crash
// in player code (RDR.exe+0x39fbc4) on the next frame.
uint32_t g_puppetOwner = 0;
std::vector<uint32_t> g_puppets;  // goh handles of spawned remote-player puppets
bool g_playerSpawned = false;
bool g_worldReady = false;  // set when the world load completed (net client connects then)
bool g_localDead      = false; // set by DeathMonitor, cleared by a script respawn
bool g_limbo         = false; // MTA-style join: player parked underground, frozen, screen black until spawnPlayer
Vec3 g_limboPos{0.0f, -300.0f, 0.0f};
bool g_limboSpawn    = true;
wchar_t g_iniPath[MAX_PATH] = L"";
// playerInfo+0x5ec is overwritten by every actor creation, so the player's actor handle is cached right after spawn.
uint32_t g_playerActorHandle = 0;
uint32_t g_playerGoh = 0;  // goh handle of the current local player (body; changes on respawn)
// Puppet "mirror" test (F10): every frame, teleport each puppet next to the player with the player's heading.
bool g_mirror = false;
// Follow test (F11): puppets walk to the player with AI go-to tasks. g_followStyle 0 = from player speed.
bool g_follow = false;
int g_followStyle = 0;
// Actor model variation (create-actor a7 / factory param_5, clamped to the def's variation count). The story script
// passes 12 for the player. -1 for puppets = cycle 0,1,2,... per F9 spawn (to compare variations).
int32_t g_playerVariation = 12;
int32_t g_puppetVariation = 12;
bool g_forceNavGate = false;  // experiment: set *(*(0x142ad3238)+0x70) |= 1 (global nav update gate)

// The create-actor routine takes the actor name as a C string pointer that must stay valid.
const char kPlayerName[] = "player";

float IniFloat(const wchar_t* ini, const wchar_t* key, float def) {
    wchar_t buf[64], defStr[64];
    swprintf_s(defStr, L"%g", def);
    GetPrivateProfileStringW(L"sandbox", key, defStr, buf, 64, ini);
    return static_cast<float>(_wtof(buf));
}

// Game-object handle table (*(0x142c45808)): 16-byte entries { object*, u16 generation, ... }.
// The create-actor factory (FUN_1402209e0) takes the handle of the Layout the actor is created into, and
// only initializes the actor (Actor::vf0 -> player controller link) if that handle is valid.
constexpr uintptr_t kGohTablePtr = 0x142C45808;
constexpr uintptr_t kLayoutVtables[] = {0x141C821C0, 0x141C8B1F0, 0x141C8B3E0};  // Layout (+ subclasses)
constexpr uintptr_t kMobileLayoutVtable = 0x141C81598;
constexpr uintptr_t kLayoutMgrVtable = 0x141C8B000;
constexpr uint32_t kMaxGohEntries = 0x10000;

const char* LayoutKind(uintptr_t vt) {
    for (uintptr_t v : kLayoutVtables)
        if (vt == GameAddr(v)) return "Layout";
    if (vt == GameAddr(kMobileLayoutVtable)) return "MobileLayout";
    if (vt == GameAddr(kLayoutMgrVtable)) return "LayoutMgr";
    return nullptr;
}

// Fault-free 8-byte read. The goh scan races the game's streaming threads: an object can be freed and its page decommitted
// between IsReadable() and the read (crashed copy 2 of two on the first puppet spawn, 2026-11-19). ReadProcessMemory on our
// own process fails instead of faulting; SEH is no good here because the game's crash reporting reacts to first-chance
// access violations (it started RDRMessage.exe even though the fault was handled).
bool TryRead64(uintptr_t addr, uintptr_t& out) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), &out, sizeof(out), &got) && got == sizeof(out);
}

// Scans the game-object table for live Layout objects, logs them, returns the first Layout's handle (0 = none).
uint32_t FindLayoutHandle() {
    const uintptr_t table = *reinterpret_cast<uintptr_t*>(GameAddr(kGohTablePtr));
    if (!table) { Log("sandbox: goh table is null"); return 0; }
    uint32_t first = 0, found = 0, live = 0;
    for (uint32_t i = 0; i < kMaxGohEntries; ++i) {
        const uintptr_t entry = table + i * 0x10;
        if ((entry & 0xFFF) == 0 || i == 0) {
            if (!IsReadable(entry, 0x10)) break;  // end of the table
        }
        uintptr_t obj = 0, genWord = 0;
        if (!TryRead64(entry, obj) || !TryRead64(entry + 8, genWord)) break;  // table end
        const uint16_t gen = static_cast<uint16_t>(genWord);
        // Free slots hold small free-list values, not pointers.
        uintptr_t vt = 0;
        const bool isObj = obj >= 0x10000 && TryRead64(obj, vt);
        if (i == 808) {
            Log("sandbox: goh[808] = obj %p gen %u vtable %p", reinterpret_cast<void*>(obj), gen, reinterpret_cast<void*>(vt));
        }
        if (!isObj) continue;
        ++live;
        const char* kind = LayoutKind(vt);
        if (!kind) continue;
        const uint32_t handle = (static_cast<uint32_t>(gen) << 16) | i;
        if (found < 24) {
            Log("sandbox: goh[%u] %s vt=%p obj=%p handle=0x%08x", i, kind, reinterpret_cast<void*>(vt),
                reinterpret_cast<void*>(obj), handle);
        }
        ++found;
        // Only the base Layout class (vt 0x141C821C0) can own actors; the subclasses at goh[316..324] can't.
        if (!first && vt == GameAddr(kLayoutVtables[0])) first = handle;
    }
    Log("sandbox: goh scan: %u live objects, %u layouts, using handle 0x%08x", live, found, first);
    return first;
}

// Game camera (RE: TransitionEventCutGameCameraBehindPlayer / GameCameraReset vf9, NOTES.md):
//   camMgr = *(0x142c45738); gameCam = camMgr+0x28
constexpr uintptr_t kCameraMgrPtr = 0x142C45738;

uintptr_t ReadPtr(uintptr_t addr) { return IsReadable(addr, 8) ? *reinterpret_cast<uintptr_t*>(addr) : 0; }

// Script threads (RE: VM runner FUN_1402066d0, scrThread::Kill FUN_1401e6710):
//   threads = *(0x142b0d1e0) (array of scrThread*), count = *(u16*)0x142b0d1e8
//   thread +0x08 program, +0x10 id, +0x14 state
constexpr uintptr_t kThreadsPtr = 0x142B0D1E0;
constexpr uintptr_t kThreadCount = 0x142B0D1E8;
constexpr uintptr_t kScrThreadKill = 0x1401E6710;

}  // namespace

void CollisionProbeTick();  // debug probe, defined below
void WalkProbeTick();
void TakeStateSnap(int i, uintptr_t actor);
uintptr_t GohObject(uint32_t goh);
uint32_t SpawnRemotePuppet(const Vec3& pos, float heading, const char* label);
void LogStateDiff();
void LogStreamGrid(const Vec3& p);  // defined below

// Kills every running script thread (sp_idle keeps driving scenic cameras otherwise). Main thread only, outside the VM.
void KillScriptThreads(const char* why) {
    const uintptr_t arr = ReadPtr(GameAddr(kThreadsPtr));
    const uint16_t count = *reinterpret_cast<uint16_t*>(GameAddr(kThreadCount));
    using KillFn = void (*)(uintptr_t thread);
    auto kill = reinterpret_cast<KillFn>(GameAddr(kScrThreadKill));
    int killed = 0;
    for (uint16_t i = 0; arr && i < count && i < 256; ++i) {
        const uintptr_t th = ReadPtr(arr + i * 8);
        if (!th || !IsReadable(th, 0x90)) continue;
        const uintptr_t prog = ReadPtr(th + 8);
        if (!prog) continue;  // unused thread slot
        Log("scripts (%s): thread[%u]=%p vt=%p id=%u state=%u program=%p", why, i, reinterpret_cast<void*>(th),
            reinterpret_cast<void*>(ReadPtr(th)), *reinterpret_cast<uint32_t*>(th + 0x10),
            *reinterpret_cast<uint32_t*>(th + 0x14), reinterpret_cast<void*>(prog));
        if (prog) {
            kill(th);
            ++killed;
        }
    }
    Log("scripts (%s): killed %d thread(s) of %u", why, killed, count);
}

// Attaches the gameplay camera to the local player, exactly like the world-load setup FUN_1401f9830 does when the
// player already exists (in the sandbox the player is spawned later, so it never happens on its own):
//   cam = camMgr+0x28 (the "GameCamera" gohCamera's camera instance)
//   FUN_140376e50(cam, playerActorHandle, 0); FUN_140376f40(cam, 0, 0); cam+0x860 = actor+0x188
// NOTE: camMgr+0x20 is the GameCamera gohCamera itself; never deactivate it (the renderer reads cam+0x860 -> crash).
constexpr uintptr_t kCamSetTarget = 0x140376E50;
constexpr uintptr_t kCamReset = 0x140376F40;
constexpr uintptr_t kPlayerInfoPtr = 0x1422DAF30;
constexpr uintptr_t kActorHandleTablePtr = 0x142C457F8;

void AttachCameraToPlayer(const char* why) {
    const uintptr_t mgr = ReadPtr(GameAddr(kCameraMgrPtr));
    const uintptr_t cam = mgr ? ReadPtr(mgr + 0x28) : 0;
    const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
    if (!cam || !info) {
        Log("camera (%s): no camera (%p) or player info (%p)", why, reinterpret_cast<void*>(cam), reinterpret_cast<void*>(info));
        return;
    }
    const uint32_t handle = g_playerActorHandle ? g_playerActorHandle : *reinterpret_cast<uint32_t*>(info + 0x5EC);
    const uintptr_t table = ReadPtr(GameAddr(kActorHandleTablePtr));
    const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
    if (!table || !IsReadable(entry, 0x10) || *reinterpret_cast<uint16_t*>(entry + 8) != static_cast<uint16_t>(handle >> 16)) {
        Log("camera (%s): player actor handle 0x%08x not valid", why, handle);
        return;
    }
    const uintptr_t actor = ReadPtr(entry);
    using SetTargetFn = void (*)(uintptr_t cam, uint32_t handle, uint32_t zero);
    using ResetFn = void (*)(uintptr_t cam, uint64_t a, uint64_t b);
    reinterpret_cast<SetTargetFn>(GameAddr(kCamSetTarget))(cam, handle, 0);
    reinterpret_cast<ResetFn>(GameAddr(kCamReset))(cam, 0, 0);
    *reinterpret_cast<uint32_t*>(cam + 0x860) = actor ? *reinterpret_cast<uint32_t*>(actor + 0x188) : 0;
    Log("camera (%s): game camera %p attached to player actor %p (handle 0x%08x)", why, reinterpret_cast<void*>(cam),
        reinterpret_cast<void*>(actor), handle);
}

// Model component mask. Right after creating the player the story script calls native FUN_1402826c0(goh, bit, 1) for
// bits 0x1a, 0x17, 0x19, 0x1b, 0x1c, 0x1d, 0x1e (rdmp_story_trace.log). The native sets/clears that bit in the u32 at
// +0x114 of *(*(*(actor+0x50)+0x88)+0x110) (disasm 0x1402826c0). Without it Marston (and puppets) have no hands.
constexpr uint32_t kStoryComponentBits = (1u << 0x17) | (1u << 0x19) | (1u << 0x1a) | (1u << 0x1b) | (1u << 0x1c) |
                                         (1u << 0x1d) | (1u << 0x1e);

void ApplyStoryComponentMask(uintptr_t actor, const char* who) {
    const uintptr_t a = actor ? ReadPtr(actor + 0x50) : 0;
    const uintptr_t b = a ? ReadPtr(a + 0x88) : 0;
    const uintptr_t c = b ? ReadPtr(b + 0x110) : 0;
    if (!c || !IsReadable(c + 0x114, 4)) {
        Log("components (%s): mask object missing (actor %p)", who, reinterpret_cast<void*>(actor));
        return;
    }
    auto* mask = reinterpret_cast<uint32_t*>(c + 0x114);
    const uint32_t before = *mask;
    *mask = before | kStoryComponentBits;
    Log("components (%s): mask 0x%08x -> 0x%08x", who, before, *mask);
}

// Live player root position: *(actor+0x40)+0x20 (found by walking + F8 diff; Y is up).
// (actor+0x88)+0x70 and (actor+0xc8)+0x70 hold the same translation inside transform matrices.
bool PlayerActorAndPos(uintptr_t& actorOut, Vec3& posOut) {
    uint32_t handle = g_playerActorHandle;
    if (!handle) {
        const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
        if (!info) return false;
        handle = *reinterpret_cast<uint32_t*>(info + 0x5EC);
    }
    const uintptr_t table = ReadPtr(GameAddr(kActorHandleTablePtr));
    const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
    if (!table || !IsReadable(entry, 0x10) || *reinterpret_cast<uint16_t*>(entry + 8) != static_cast<uint16_t>(handle >> 16)) return false;
    actorOut = ReadPtr(entry);
    // *(actor+0x40)+0x20 is NOT per-actor (it showed the last puppet's spawn point); use the transform matrix
    // translation *(actor+0xc8)+0x70.
    const uintptr_t xf = actorOut ? ReadPtr(actorOut + 0xc8) : 0;
    if (!xf || !IsReadable(xf + 0x70, 12)) return false;
    posOut = *reinterpret_cast<Vec3*>(xf + 0x70);
    return true;
}

// Player control: the hogtie handlers in the game-flow event handler toggle a "controls disabled" bitfield on the
// player's controller component (actor+0x70): putdown does |= 0x7d, pickup does &= 0x82. Clear it like pickup.
void EnablePlayerControl(const char* why) {
    const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
    if (!info) return;
    const uint32_t handle = g_playerActorHandle ? g_playerActorHandle : *reinterpret_cast<uint32_t*>(info + 0x5EC);
    const uintptr_t table = ReadPtr(GameAddr(kActorHandleTablePtr));
    const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
    if (!table || !IsReadable(entry, 0x10) || *reinterpret_cast<uint16_t*>(entry + 8) != static_cast<uint16_t>(handle >> 16)) return;
    const uintptr_t actor = ReadPtr(entry);
    const uintptr_t comp = actor ? ReadPtr(actor + 0x70) : 0;
    if (!comp || !IsReadable(comp + 0x48e, 1)) {
        Log("control (%s): no controller component (actor %p)", why, reinterpret_cast<void*>(actor));
        return;
    }
    auto* flags = reinterpret_cast<uint8_t*>(comp + 0x48e);
    const uint8_t before = *flags;
    *flags = static_cast<uint8_t>(before & 0x82);
    Log("control (%s): controller %p flags 0x%02x -> 0x%02x", why, reinterpret_cast<void*>(comp), before, *flags);
}

// MTA-style join: the player has no control until the script spawns them (putdown semantics, |= 0x7d).
void DisablePlayerControl(const char* why) {
    const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
    if (!info) return;
    const uint32_t handle = g_playerActorHandle ? g_playerActorHandle : *reinterpret_cast<uint32_t*>(info + 0x5EC);
    const uintptr_t table = ReadPtr(GameAddr(kActorHandleTablePtr));
    const uintptr_t entry = table + (handle & 0xFFFF) * 0x10;
    if (!table || !IsReadable(entry, 0x10) || *reinterpret_cast<uint16_t*>(entry + 8) != static_cast<uint16_t>(handle >> 16)) return;
    const uintptr_t actor = ReadPtr(entry);
    const uintptr_t comp = actor ? ReadPtr(actor + 0x70) : 0;
    if (!comp || !IsReadable(comp + 0x48e, 1)) return;
    auto* flags = reinterpret_cast<uint8_t*>(comp + 0x48e);
    *flags = static_cast<uint8_t>(*flags | 0x7d);
    Log("control (%s): disabled", why);
}

// While in limbo the player is invulnerable: hold the health component at 100.
void LimboTick(float) {
    if (!g_limbo || !g_playerActorHandle) return;
    uintptr_t a = 0;
    Vec3 p{};
    if (!PlayerActorAndPos(a, p)) return;
    const uintptr_t hc = ReadPtr(a + 0x60);
    if (hc && IsReadable(hc + 0x20, 4)) *reinterpret_cast<float*>(hc + 0x20) = 100.0f;
}


// Does what the story's end-of-load camera transitions do: reset the game camera and cut it behind the player.
void CameraToPlayer(const char* why) {
    const uintptr_t mgr = ReadPtr(GameAddr(kCameraMgrPtr));
    const uintptr_t cam = mgr ? ReadPtr(mgr + 0x28) : 0;
    Log("camera (%s): camMgr=%p vt=%p gameCam=%p vt=%p", why, reinterpret_cast<void*>(mgr),
        reinterpret_cast<void*>(mgr ? ReadPtr(mgr) : 0), reinterpret_cast<void*>(cam),
        reinterpret_cast<void*>(cam ? ReadPtr(cam) : 0));
    if (!cam) return;
    // TransitionEventGameCameraReset (DesiredZDriftDistance = 0)
    *reinterpret_cast<float*>(cam + 0x1b60) = 0.0f;
    *reinterpret_cast<uint8_t*>(cam + 0x1c7b) = 1;
    *reinterpret_cast<uint32_t*>(cam + 0x1b64) = 0;
    // TransitionEventCutGameCameraBehindPlayer
    const uintptr_t a = ReadPtr(cam + 0x620);
    const uintptr_t b = a ? ReadPtr(a + 0x30) : 0;
    const uintptr_t c = b ? ReadPtr(b + 8) : 0;
    const uintptr_t flag = c ? ReadPtr(c + 0x2e8) : 0;
    if (flag && IsReadable(flag, 1)) {
        *reinterpret_cast<uint8_t*>(flag) = 1;
        Log("camera (%s): reset + cut behind player", why);
    } else {
        Log("camera (%s): reset done, cut-behind chain missing (%p %p %p)", why, reinterpret_cast<void*>(a),
            reinterpret_cast<void*>(b), reinterpret_cast<void*>(c));
    }
}

void EngineFadeToBlack(float seconds, const char* why);  // engine fade, defined below
namespace {

void EnsureTypeAnchor();  // defined below

void SpawnPlayer() {
    uint32_t layout = g_ownerHandle;
    if (!layout) layout = FindLayoutHandle();  // owner_handle=0: first Layout (debug/experiments)
    if (!layout) {
        Log("sandbox: no Layout object found, can't spawn the player");
        return;
    }
    const Vec3 rot{0.0f, g_spawnHeading, 0.0f};
    const uint32_t h = CreateActor(layout, reinterpret_cast<uint64_t>(kPlayerName), 0, g_spawnPos, rot, g_playerVariation);
    Log("sandbox: spawned player into layout 0x%08x at (%.2f, %.2f, %.2f) -> handle 0x%08x", layout, g_spawnPos.x,
        g_spawnPos.y, g_spawnPos.z, h);
    g_playerGoh = h;
    if (const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr))) {
        g_playerActorHandle = *reinterpret_cast<uint32_t*>(info + 0x5EC);
        Log("sandbox: cached player actor handle 0x%08x", g_playerActorHandle);
        uintptr_t pa = 0;
        Vec3 pp{};
        if (PlayerActorAndPos(pa, pp)) {
            ApplyStoryComponentMask(pa, "player");
            SetLocoPlayerActor(pa);
            // debug: who writes the locomotion move value *(actor+0x38)+0xab0 (rdmp.ini [debug] watch_move=1)
            if (GetPrivateProfileIntW(L"debug", L"watch_move", 0, g_iniPath) && ReadPtr(pa + 0x38))
                ArmWriteWatch(ReadPtr(pa + 0x38) + 0xab0, "moveValue");
            const int probe = GetPrivateProfileIntW(L"debug", L"collision_probe", 0, g_iniPath);
            if (probe == 1) RunOnMainThread([] { CollisionProbeTick(); });
            if (probe == 2) RunOnMainThread([] { WalkProbeTick(); });
        }
    }
    for (uint32_t i = 0; i < g_puppetsAtSpawn; ++i) SpawnPuppetNearPlayer();  // test: same context as the player
    { const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr)); Log("sandbox: after boot spawn: DAT_14241e310=0x%08x +0x5e8=%02x %02x %02x %02x +0x104=%04x +0x10c=%02x %02x", *reinterpret_cast<uint32_t*>(GameAddr(0x14241E310)), info ? *reinterpret_cast<uint8_t*>(info + 0x5e8) : 0, info ? *reinterpret_cast<uint8_t*>(info + 0x5e9) : 0, info ? *reinterpret_cast<uint8_t*>(info + 0x5ea) : 0, info ? *reinterpret_cast<uint8_t*>(info + 0x5eb) : 0, info ? *reinterpret_cast<uint16_t*>(info + 0x104) : 0, info ? *reinterpret_cast<uint8_t*>(info + 0x10c) : 0, info ? *reinterpret_cast<uint8_t*>(info + 0x10d) : 0); }
    if (g_killScripts) KillScriptThreads("after spawn");
    AttachCameraToPlayer("after spawn");
    CameraToPlayer("after spawn");
    EnablePlayerControl("after spawn");
    HideLoadingOverlay(1500);
    if (g_engineFade) {  // hold engine black while the camera settles behind the player, then fade in (FadeHoldFrame)
        EngineFadeToBlack(0.0f, "player spawn");
        g_fadeInAt = GetTickCount64() + g_fadeHoldMs;
    }
    // (EnsureTypeAnchor() not needed: the respawn failure was the player owner, not the template type)
}

// ---- local player death + respawn ----
// RE (alive/dead state diff, NOTES.md): health component = *(actor+0x60): +0x20 float health (100 alive, 0 dead),
// +0x18 u32 state (0 alive, 2 dead). The sandbox has no story script, so nothing respawns a dead player: after
// respawn_delay_ms we create a fresh player (same "create as player" path the boot uses: factory with asPlayer=1 into
// the player owner goh[808]), redo the post-spawn setup, and the old body is released (goh Actor vf2).
uint32_t g_respawnDelayMs = 5000;
int g_respawns = 0;

// After a player creation: cache the actor handle (playerInfo+0x5ec), hands, locomotion capture, camera, controls.
void SetupLocalPlayer(const char* why) {
    if (const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr))) g_playerActorHandle = *reinterpret_cast<uint32_t*>(info + 0x5EC);
    uintptr_t pa = 0;
    Vec3 pp{};
    if (PlayerActorAndPos(pa, pp)) {
        ApplyStoryComponentMask(pa, why);
        SetLocoPlayerActor(pa);
    }
    AttachCameraToPlayer(why);
    CameraToPlayer(why);
    EnablePlayerControl(why);
}

uint32_t g_releasedBody = 0;  // old player body released by phase 1

// Type anchor: a hidden, non-player actor of the player template (def 0) parked far below the map. The factory resolves
// the template's actor type in a registry (DAT_142c457a8, "template_player" + "eActorType"); when the last actor of the
// type is released the type unloads and the factory returns 0 (respawn failed / the new player was invisible). Keeping
// one anchor alive keeps the type loaded.
uint32_t g_typeAnchor = 0;

void EnsureTypeAnchor() {
    if (g_typeAnchor && GohObject(g_typeAnchor)) return;
    const Vec3 at{g_spawnPos.x, g_spawnPos.y - 500.0f, g_spawnPos.z};
    g_typeAnchor = SpawnRemotePuppet(at, 0.0f, "type anchor");
    Log("respawn: type anchor goh 0x%08x (def 0 kept loaded)", g_typeAnchor);
}

// Phase 1: release the dead body. The player owner (goh[808]) holds only one player: the factory returns 0 while the old
// one exists. Creating in the same frame as the release left the new player invisible (shared model reloading).
void ReleaseDeadPlayer() {
    g_releasedBody = g_playerGoh;
    if (g_playerGoh) StreamOutPuppet(g_playerGoh);
    g_playerGoh = 0;
}

// Phase 2 (a moment later): create the new player.
void RespawnPlayer() {
    const uint32_t oldGoh = g_releasedBody;
    // The player owner goh[808] holds only one actor for good once its player was created (the factory's object
    // allocation FUN_140235cd0 returns null there even after the body is released), so respawned players are created
    // into an ordinary Layout, still flagged as the player (factory asPlayer=1 -> FUN_140ae3ff0 links playerInfo+0x5ec).
    static uint32_t baseLayout = 0;
    if (!baseLayout) baseLayout = FindLayoutHandle();
    if (!baseLayout) return;
    static char names[64][32];
    char* name = names[g_respawns % 64];
    sprintf_s(names[g_respawns % 64], "player_r%d", g_respawns + 1);
    ++g_respawns;
    const Vec3 rot{0.0f, g_spawnHeading, 0.0f};
    const uint32_t h = CreateActorDirect(baseLayout, reinterpret_cast<uint64_t>(name), 0, g_spawnPos, rot, g_playerVariation, 1);
    Log("respawn: \"%s\" into Layout 0x%08x at (%.2f, %.2f, %.2f) -> goh 0x%08x (old body goh 0x%08x released)", name, baseLayout,
        g_spawnPos.x, g_spawnPos.y, g_spawnPos.z, h, oldGoh);
    if (!h) return;
    g_playerGoh = h;
    // Open issue (HANDOFF.md): after a death the screen keeps showing the old body until the game reads any keyboard
    // input; not yet understood.
    SetupLocalPlayer("respawn");
    // debug (collision_probe=2): run the river walk once more to test a second death/respawn
    if (g_respawns < 2 && GetPrivateProfileIntW(L"debug", L"collision_probe", 0, g_iniPath) == 2) RunOnMainThread([] { WalkProbeTick(); });
}

void DeathMonitor(float) {
    // 0 = alive, 1 = dead (waiting respawn_delay_ms), 2 = body released (waiting for the model to settle)
    static int phase = 0;
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    if (phase == 2) {
        if (now - since >= 1500) {
            phase = 0;
            RespawnPlayer();
        }
        return;
    }
    if (!g_playerActorHandle) return;
    uintptr_t a = 0;
    Vec3 p{};
    if (!PlayerActorAndPos(a, p)) return;
    const uintptr_t hc = ReadPtr(a + 0x60);
    if (!hc || !IsReadable(hc + 0x18, 0xc)) return;
    const float health = *reinterpret_cast<float*>(hc + 0x20);
    if (health > 0.0f) {  // (state +0x18 == 2 also occurs while alive, e.g. on a slope / in water)
        phase = 0;
        return;
    }
    if (phase == 0) {
        phase = 1;
        since = now;
        g_localDead = true;
        if (g_respawnDelayMs == 0) {
            // script mode: the server decides when to respawn (spawnPlayer native)
            Log("death: player died at (%.1f, %.1f, %.1f) (health %.1f, state %u); waiting for a script respawn", p.x, p.y, p.z,
                health, *reinterpret_cast<uint32_t*>(hc + 0x18));
        }
        else {
            Log("death: player died at (%.1f, %.1f, %.1f) (health %.1f, state %u); respawning in %u ms", p.x, p.y, p.z, health,
                *reinterpret_cast<uint32_t*>(hc + 0x18), g_respawnDelayMs);
        }
    } else if (phase == 1 && g_respawnDelayMs != 0 && now - since >= g_respawnDelayMs) {
        ReleaseDeadPlayer();
        phase = 2;
        since = now;
    }
}

}  // namespace

// Remote-player prototype: a second Marston created into an ordinary Layout (not the local player's owner),
// 3 m from the player. Main thread only (use RunOnMainThread).
// Actor definitions (RE: create-actor factory FUN_1402209e0): defs = *(0x142b12070) (array of def*),
// count = *(u16*)0x142b12078; def[2] = asset name string. a2 of create-actor = def index (0 = the player def).
void LogActorDefs() {
    static bool done = false;
    if (done) return;
    done = true;
    const uintptr_t arr = ReadPtr(GameAddr(0x142B12070));
    const uint16_t count = *reinterpret_cast<uint16_t*>(GameAddr(0x142B12078));
    Log("actor defs: %u", count);
    for (uint16_t i = 0; arr && i < count && i < 2000; ++i) {
        const uintptr_t def = ReadPtr(arr + i * 8);
        const uintptr_t name = def ? ReadPtr(def + 0x10) : 0;
        char buf[96] = "";
        if (name && IsReadable(name, 1)) {
            const char* s = reinterpret_cast<const char*>(name);
            size_t k = 0;
            for (; k + 1 < sizeof(buf) && IsReadable(name + k, 1) && s[k] >= 0x20 && s[k] < 0x7f; ++k) buf[k] = s[k];
            buf[k] = '\0';
        }
        Log("  def[%u] = %p \"%s\"", i, reinterpret_cast<void*>(def), buf);
    }
}

uintptr_t ActorFromGohHandle(uint32_t gohHandle);  // defined below
bool GiveGoToCoord(uintptr_t actor, const Vec3& target, int style);  // defined below

void SpawnPuppetNearPlayer() {
    Log("puppet: DAT_14241e310 = 0x%04x", *reinterpret_cast<uint16_t*>(GameAddr(0x14241E310)));
    uintptr_t actor = 0;
    Vec3 pos{};
    if (!PlayerActorAndPos(actor, pos)) {
        Log("puppet: no player position");
        return;
    }
    // Owner: the same static owner object the player uses (goh[808]); ordinary Layouts reject runtime spawns.
    const uint32_t layout = g_puppetOwner ? g_puppetOwner : FindLayoutHandle();
    if (!layout) return;
    // In front of the player at spawn (the camera starts looking down -Z).
    const Vec3 spawn{pos.x, pos.y, pos.z - 4.0f};
    const Vec3 rot{0.0f, 0.0f, 0.0f};
    // Each actor needs its own name (the "player" name is taken). Names must stay valid: keep them in a static pool.
    static char names[64][32];
    static int next = 0;
    char* name = names[next % 64];
    sprintf_s(names[next % 64], "rdmp_remote_%d", next + 1);
    ++next;
    // Must be a non-player template (e.g. 3 = template_base_human). Player templates (0 template_player,
    // 1 _jack, 2 _cs) crash the player-controller update (FUN_14039fbc0 via FUN_14034a6d0) without player info.
    static int attempt = -1;
    const uint32_t def = g_puppetDef;
    ++attempt;
    const Vec3 at{spawn.x - 3.0f + 1.2f * static_cast<float>(attempt % 6), spawn.y, spawn.z};
    const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
    const uint8_t gate = info ? *reinterpret_cast<uint8_t*>(info + 0x5e9) : 0xff;
    const uint32_t h = CreateActorDirect(layout, reinterpret_cast<uint64_t>(name), def, at, rot, g_puppetVariation < 0 ? attempt : g_puppetVariation);
    {
        const uintptr_t defObj = ReadPtr(ReadPtr(GameAddr(0x142B12070)) + def * 8);
        const int nVar = defObj ? reinterpret_cast<int (*)(uintptr_t)>(ReadPtr(ReadPtr(defObj) + 0x90))(defObj) : -1;
        Log("puppet: def %u has %d variation(s); using %d", def, nVar, g_puppetVariation < 0 ? attempt : g_puppetVariation);
    }
    Log("puppet: name \"%s\" def %u owner 0x%x at (%.2f, %.2f, %.2f) [playerInfo+0x5e9=%u] -> handle 0x%08x", name, def,
        layout, at.x, at.y, at.z, gate, h);
    if (h) {
        g_puppets.push_back(h);
        ApplyStoryComponentMask(ActorFromGohHandle(h), "puppet");
    }
    Log("puppet: player at (%.2f, %.2f, %.2f), spawned puppet into layout 0x%08x at (%.2f, %.2f, %.2f) -> handle 0x%08x",
        pos.x, pos.y, pos.z, layout, spawn.x, spawn.y, spawn.z, h);
}

// ---- puppet movement: engine teleport ----
// Puppet goh handle -> goh object (table *(0x142c45808)) -> actor handle at goh+0xb0 -> actor (table *(0x142c457f8)).
// Position lives in three places: *(actor+0x40)+0x20, *(actor+0x88)+0x70, *(actor+0xc8)+0x70 (found with F8 diff).

uintptr_t ActorFromGohHandle(uint32_t gohHandle) {
    const uintptr_t gohTable = ReadPtr(GameAddr(kGohTablePtr));
    const uintptr_t ge = gohTable + (gohHandle & 0xFFFF) * 0x10;
    if (!gohTable || !IsReadable(ge, 0x10) || *reinterpret_cast<uint16_t*>(ge + 8) != static_cast<uint16_t>(gohHandle >> 16)) return 0;
    const uintptr_t goh = ReadPtr(ge);
    if (!goh || !IsReadable(goh + 0xb0, 4)) return 0;
    const uint32_t ah = *reinterpret_cast<uint32_t*>(goh + 0xb0);
    const uintptr_t actorTable = ReadPtr(GameAddr(kActorHandleTablePtr));
    const uintptr_t ae = actorTable + (ah & 0xFFFF) * 0x10;
    if (!actorTable || !IsReadable(ae, 0x10) || *reinterpret_cast<uint16_t*>(ae + 8) != static_cast<uint16_t>(ah >> 16)) return 0;
    return ReadPtr(ae);
}

uint32_t ActorHandleFromGohHandle(uint32_t gohHandle) {
    const uintptr_t gohTable = ReadPtr(GameAddr(kGohTablePtr));
    const uintptr_t ge = gohTable + (gohHandle & 0xFFFF) * 0x10;
    if (!gohTable || !IsReadable(ge, 0x10) || *reinterpret_cast<uint16_t*>(ge + 8) != static_cast<uint16_t>(gohHandle >> 16)) return 0;
    const uintptr_t goh = ReadPtr(ge);
    return goh && IsReadable(goh + 0xb0, 4) ? *reinterpret_cast<uint32_t*>(goh + 0xb0) : 0;
}

// Engine teleport (RE: TransitionEventTeleportWithHeading::vf9 @ 0x1404025a0):
//   FUN_140ad4ca0(actorHandle, const Vec3* pos, float headingDegrees, 1, 0, 0)
// Validates the handle against the actor table, builds a Y-axis rotation matrix + translation and applies it through
// FUN_140ad4680 (which updates physics/spatial state; raw matrix writes don't).
constexpr uintptr_t kTeleportActor = 0x140AD4CA0;

void TeleportActor(uint32_t actorHandle, const Vec3& pos, float headingDeg, uint8_t warp = 1) {
    using Fn = void (*)(uint64_t handle, const float* pos, float heading, uint8_t a, uint8_t b, uint8_t c);
    reinterpret_cast<Fn>(GameAddr(kTeleportActor))(actorHandle, &pos.x, headingDeg, warp, 0, 0);
}

// Heading (degrees) from an actor's transform *(actor+0xc8): rows at +0x40 (right) ... +0x70 (translation).
// Teleport builds row0 = (cos h, 0, -sin h), so h = atan2(-row0.z, row0.x).
float ActorHeadingDeg(uintptr_t actor) {
    const uintptr_t xf = actor ? ReadPtr(actor + 0xc8) : 0;
    if (!xf || !IsReadable(xf + 0x40, 0x40)) return 0.0f;
    const float* r0 = reinterpret_cast<const float*>(xf + 0x40);
    return std::atan2(-r0[2], r0[0]) * 57.29578f;
}

// Mirror test: each frame every puppet is teleported to a point beside the player (spread out in a row, rotated with the
// player) and gets the player's heading. Stand-in for applying network snapshots.
void MirrorTick() {
    if (!g_mirror) return;
    uintptr_t player = 0;
    Vec3 c{};
    if (PlayerActorAndPos(player, c)) {
        const float h = ActorHeadingDeg(player);
        static int ticks = 0;
        ++ticks;
        for (size_t i = 0; i < g_puppets.size(); ++i) {
            const uint32_t ah = ActorHandleFromGohHandle(g_puppets[i]);
            if (!ah) continue;
            // Fixed world offset: 4 m toward -Z (in front of the spawn camera), spread along X.
            const Vec3 target{c.x - 1.0f + 2.0f * static_cast<float>(i), c.y, c.z - 4.0f};
            {
                // Teleport only to correct drift (per-frame teleports reset locomotion); the copied desire moves it.
                const uintptr_t pa0 = ActorFromGohHandle(g_puppets[i]);
                const uintptr_t xf0 = pa0 ? ReadPtr(pa0 + 0xc8) : 0;
                Vec3 cur0{};
                if (xf0 && IsReadable(xf0 + 0x70, 12)) cur0 = *reinterpret_cast<Vec3*>(xf0 + 0x70);
                const float dx = cur0.x - target.x, dz = cur0.z - target.z;
                if (ticks == 1 || dx * dx + dz * dz > 1.5f * 1.5f) TeleportActor(ah, target, h);
            }
            {
                // Locomotion: give the puppet the player's own movement desire (locomotion.cpp hook) so the engine
                // plays the same walk/run cycle while we teleport it. Puppets need an AI brain for the brain update.
                const uintptr_t pupActor = ActorFromGohHandle(g_puppets[i]);
                if (pupActor && !ReadPtr(pupActor + 0x28)) {
                    GiveGoToCoord(pupActor, target, 1);  // creates the AI brain on demand (FUN_140af93c0)
                    Log("mirror: puppet %zu had no brain -> now %p", i, reinterpret_cast<void*>(ReadPtr(pupActor + 0x28)));
                }
                static uint8_t desire[kDesireSize];
                uint32_t seq = 0;
                if (pupActor && GetPlayerDesire(desire, &seq)) SetPuppetDesire(pupActor, desire);
                if (i == 0 && ticks % 240 == 1) {
                    const float* d = reinterpret_cast<const float*>(desire);
                    {
                        const uint32_t* w = reinterpret_cast<const uint32_t*>(desire);
                        char line[1024];
                        int n = 0;
                        for (int k = 0; k < static_cast<int>(kDesireSize / 4) && n < 1000; ++k) n += sprintf_s(line + n, sizeof(line) - n, "%08x ", w[k]);
                        if (ticks < 2000) Log("mirror: desire raw %s", line);
                    }
                    const uintptr_t pc = ReadPtr(pupActor + 0x38), plc = ReadPtr(player + 0x38);
                    Log("mirror: puppet +0x38+0xab0=%.3f +0xab4=%.3f | player %.3f %.3f", pc ? *reinterpret_cast<float*>(pc + 0xab0) : -9.f, pc ? *reinterpret_cast<float*>(pc + 0xab4) : -9.f, plc ? *reinterpret_cast<float*>(plc + 0xab0) : -9.f, plc ? *reinterpret_cast<float*>(plc + 0xab4) : -9.f);
                    Log("mirror: desire seq %u applied %u | [10..17] %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | [20..22] %.2f %.2f %.2f | [34] %.2f [39] %.2f [3a] %.2f",
                        seq, LocoOverridesApplied(), d[0x10], d[0x11], d[0x12], d[0x13], d[0x14], d[0x15], d[0x16], d[0x17], d[0x20],
                        d[0x21], d[0x22], d[0x34], d[0x39], d[0x3a]);
                }
            }
            if (ticks == 1 || ticks % 120 == 0) {
                Vec3 now{};
                const uintptr_t a = ActorFromGohHandle(g_puppets[i]);
                const uintptr_t xf = a ? ReadPtr(a + 0xc8) : 0;
                if (xf && IsReadable(xf + 0x70, 12)) now = *reinterpret_cast<Vec3*>(xf + 0x70);
                Log("mirror: tick %d puppet %zu -> (%.2f, %.2f, %.2f) now (%.2f, %.2f, %.2f) heading %.1f; player (%.2f, %.2f, %.2f)",
                    ticks, i, target.x, target.y, target.z, now.x, now.y, now.z, h, c.x, c.y, c.z);
            }
        }
    }
    RunOnMainThread(MirrorTick);  // again next frame
}

void TogglePuppetOrbit() {
    g_mirror = !g_mirror;
    if (g_mirror) g_follow = false;
    uintptr_t player = 0;
    Vec3 c{};
    const bool ok = PlayerActorAndPos(player, c);
    Log("puppets: mirror %s (%zu puppets), player actor %p heading %.1f", g_mirror ? "on" : "off", g_puppets.size(),
        reinterpret_cast<void*>(ok ? player : 0), ok ? ActorHeadingDeg(player) : 0.0f);
    for (size_t i = 0; i < g_puppets.size(); ++i) {
        Log("puppets: [%zu] goh 0x%08x -> actor handle 0x%08x actor %p", i, g_puppets[i],
            ActorHandleFromGohHandle(g_puppets[i]), reinterpret_cast<void*>(ActorFromGohHandle(g_puppets[i])));
    }
    if (g_mirror) RunOnMainThread(MirrorTick);
}

void LogAiState(const char* label, uintptr_t actor) {
    if (!actor || !IsReadable(actor + 0x118, 1)) return;
    const uintptr_t brain = ReadPtr(actor + 0x28);
    Log("ai: %s actor %p brain(+0x28)=%p flags(+0x118)=0x%02x brainFlags(+0x158a)=0x%02x", label,
        reinterpret_cast<void*>(actor), reinterpret_cast<void*>(brain), *reinterpret_cast<uint8_t*>(actor + 0x118),
        brain && IsReadable(brain + 0x158a, 1) ? *reinterpret_cast<uint8_t*>(brain + 0x158a) : 0xff);
    // Script-task slot (FUN_140b385e0's param_1 = brain+0x1440): +0x108 -> holder {task*, u8 active}, +0x120 pending,
    // +0x100 -> brain core (+0x159c flags).
    const uintptr_t slot = brain ? brain + 0x1440 : 0;
    if (!slot || !IsReadable(slot, 0x128)) return;
    const uintptr_t holder = ReadPtr(slot + 0x108);
    const uintptr_t task = holder ? ReadPtr(holder) : 0;
    const uintptr_t core = ReadPtr(slot + 0x100);
    Log("ai:   slot holder=%p task=%p vt=%p active=%u pending=%u/%u core=%p flags159c=0x%08x", reinterpret_cast<void*>(holder),
        reinterpret_cast<void*>(task), reinterpret_cast<void*>(task ? ReadPtr(task) : 0),
        holder && IsReadable(holder + 8, 1) ? *reinterpret_cast<uint8_t*>(holder + 8) : 0xff,
        *reinterpret_cast<uint8_t*>(slot + 0x120), *reinterpret_cast<uint8_t*>(slot + 0x121), reinterpret_cast<void*>(core),
        core && IsReadable(core + 0x159c, 4) ? *reinterpret_cast<uint32_t*>(core + 0x159c) : 0);
    // Brain update FUN_140b111a0 (rcx = core = brain+0x30) starts the script task (holder vf 0x30) only when
    // core+0x155a bit0 && !bit1, slot pending, *(slot+0x110)[0] == 0, (slot+0x11c != 2 || [1] == 0), core+0x155d != 0.
    if (core && IsReadable(core + 0x1550, 0x30)) {
        const uintptr_t st = ReadPtr(slot + 0x110);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(core + 0x1550);
        Log("ai:   gate: core+0x155a=0x%02x +0x155d=%u +0x156c=%u +0x156d=%u slot+0x11c=%u state[0]=%u state[1]=%u "
            "core+0x980=%.1f now=%.1f | core+0x1550: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            b[0xa], b[0xd], b[0x1c], b[0x1d], *reinterpret_cast<uint32_t*>(slot + 0x11c),
            st && IsReadable(st, 2) ? reinterpret_cast<uint8_t*>(st)[0] : 0xff,
            st && IsReadable(st, 2) ? reinterpret_cast<uint8_t*>(st)[1] : 0xff, *reinterpret_cast<float*>(core + 0x980),
            *reinterpret_cast<float*>(GameAddr(0x142314D3C)), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
            b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    // Navigation request object used by the go-to task (FUN_140bd4ac0: FUN_140b732c0(ctx+0xeb0, req); status = vf 0x18).
    if (false && core && IsReadable(core + 0xeb0, 0x40)) {  // wrong object (ctx isn't core); kept for reference
        const uint32_t* n = reinterpret_cast<const uint32_t*>(core + 0xeb0);
        Log("ai:   nav@core+0xeb0 vt=%p | %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x; core+0xfd0=%p",
            reinterpret_cast<void*>(ReadPtr(core + 0xeb0)), n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[10], n[11],
            n[12], n[13], reinterpret_cast<void*>(ReadPtr(core + 0xfd0)));
    }
    if (task && IsReadable(task + 0x20, 12)) {
        const float* t = reinterpret_cast<const float*>(task + 0x20);
        Log("ai:   task style=%d target=(%.2f, %.2f, %.2f)", *reinterpret_cast<int32_t*>(task + 8), t[0], t[1], t[2]);
    }
}

// ---- puppet movement via the AI task system (TASK_GO_TO_COORD), applied directly to the actor's brain ----
// RE (decomp/task_ctors.c, task_apply.c, task_queue.c):
//  - script native FUN_140c1a000 builds a task {vt_TASK_GO_TO_COORD @0x141D2C7B8, +8 i32 move style (1..7, 6 invalid),
//    +0x10 u8 0, +0x20 Vec3 target} and hands it to FUN_140af93c0(builder, actorHandle).
//  - FUN_140af93c0 only applies it to the actor when builder+0x10 (open task sequence) is null; then it calls
//    FUN_140b385e0(brain + 0x1440, task, -1, 0) with brain = actor+0x28 (skipped if brain+0x158a & 2).
//  - FUN_140b385e0 memcpy's 0xb0 bytes of the task into the brain's task slot (so a stack buffer is fine) and
//    doesn't restart an identical running task (FUN_140b38440 compares type + params).
constexpr uintptr_t kVtTaskGoToCoord = 0x141D2C7B8;
constexpr uintptr_t kBrainSetTask = 0x140B385E0;

// FUN_140af93c0(builder, actorHandle): with builder[2] (open sequence) == null it applies builder[0] (the task) to the
// actor; if the actor has no AI brain yet (actor+0x28 == 0, e.g. template_player puppets) it first creates the AI
// components ("grunt" etc.) when the AI pool allows it (DAT_142ae06d0+0xb). On failure it calls the task's vf0(task, 0).
constexpr uintptr_t kApplyTaskToActor = 0x140AF93C0;

bool GiveGoToCoord(uintptr_t actor, const Vec3& target, int style) {
    if (!actor || !IsReadable(actor + 0x118, 1)) return false;
    const uint32_t actorHandle = *reinterpret_cast<uint32_t*>(actor + 8);
    alignas(16) uint8_t task[0xb0] = {};
    *reinterpret_cast<uintptr_t*>(task + 0x00) = GameAddr(kVtTaskGoToCoord);
    *reinterpret_cast<int32_t*>(task + 0x08) = style;
    *reinterpret_cast<Vec3*>(task + 0x20) = target;
    alignas(16) uint64_t builder[4] = {reinterpret_cast<uint64_t>(task), 0, 0, 0};
    using ApplyFn = void (*)(uint64_t* builder, uint64_t actorHandle);
    reinterpret_cast<ApplyFn>(GameAddr(kApplyTaskToActor))(builder, actorHandle);
    return ReadPtr(actor + 0x28) != 0;  // has a brain now (task was queued)
}

// Follow test (F11): each puppet walks/runs (AI locomotion, real animations) to a slot 4 m in front of the player;
// far away -> teleport. Stand-in for "drive the remote ped toward the latest network position".

struct FollowState {
    Vec3 issued{1e9f, 0, 0};
    ULONGLONG issuedMs = 0;
};
FollowState g_followState[64];

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void FollowTick() {
    if (!g_follow) return;
    static ULONGLONG lastMs = 0;
    static Vec3 lastPlayer{};
    static float speed = 0.0f;
    const ULONGLONG now = GetTickCount64();
    uintptr_t player = 0;
    Vec3 c{};
    if (now - lastMs >= 100 && PlayerActorAndPos(player, c)) {  // 10 Hz (the VM hook runs several times per frame)
        if (lastMs) speed = 0.7f * speed + 0.3f * (Dist(c, lastPlayer) * 1000.0f / static_cast<float>(now - lastMs));
        lastMs = now;
        lastPlayer = c;
        const int style = g_followStyle ? g_followStyle : (speed < 2.6f ? 1 : (speed < 5.0f ? 2 : 3));
        for (size_t i = 0; i < g_puppets.size() && i < 64; ++i) {
            const uintptr_t a = ActorFromGohHandle(g_puppets[i]);
            const uintptr_t xf = a ? ReadPtr(a + 0xc8) : 0;
            if (!xf || !IsReadable(xf + 0x70, 12)) continue;
            const Vec3 cur = *reinterpret_cast<Vec3*>(xf + 0x70);
            // experiment: target 4 m BEHIND the player (puppets spawn 4 m in front -> ~8 m walk)
            const Vec3 target{c.x - 1.0f + 2.0f * static_cast<float>(i), c.y, c.z + 4.0f};
            FollowState& st = g_followState[i];
            const float d = Dist(cur, target);
            static ULONGLONG lastPosLog = 0;
            if (i == 0 && now - lastPosLog >= 1000) {
                lastPosLog = now;
                Log("follow: puppet 0 pos (%.2f, %.2f, %.2f) heading %.1f, target d=%.2f", cur.x, cur.y, cur.z, ActorHeadingDeg(a), d);
            }
            if (d > 12.0f) {
                TeleportActor(ActorHandleFromGohHandle(g_puppets[i]), target, ActorHeadingDeg(player));
                st.issued = Vec3{1e9f, 0, 0};
                Log("follow: puppet %zu too far (%.1f m) -> teleport", i, d);
            } else if (Dist(st.issued, target) > 3.0f || now - st.issuedMs > 3000) {  // re-issuing restarts the path
                const bool ok = GiveGoToCoord(a, target, style);
                st.issued = target;
                st.issuedMs = now;
                static int logged = 0;
                if (logged++ < 12 || logged % 20 == 0) {
                    if (i == 0) {
                        Log("follow: exec watch hits so far: %d", WatchHitCount());
                        static bool stateWatch = false;
                        if (false && !stateWatch && WatchExecRcx(0)) {  // (verified: state 0->1->2->0 per request)
                            stateWatch = true;
                            ArmWriteWatch(WatchExecRcx(0) + 0x350, "navState");
                        }
                        // Global nav gate: the nav object update FUN_140b736b0 returns early unless
                        // *(u8*)(*(0x142ad3238) + 0x70) & 1 (disasm: TEST byte ptr [RAX+0x70],1 at 0x140b7378b).
                        const uintptr_t navMgr = ReadPtr(GameAddr(0x142AD3238));
                        if (navMgr && IsReadable(navMgr + 0x70, 1)) {
                            auto* gate = reinterpret_cast<uint8_t*>(navMgr + 0x70);
                            Log("follow:   nav gate *(0x142ad3238)=%p +0x70 = 0x%02x%s", reinterpret_cast<void*>(navMgr), *gate,
                                g_forceNavGate && !(*gate & 1) ? " -> forcing bit 0" : "");
                            if (g_forceNavGate) *gate = static_cast<uint8_t>(*gate | 1);
                        } else {
                            Log("follow:   nav gate object missing (%p)", reinterpret_cast<void*>(navMgr));
                        }
                        // nav objects (rcx of FUN_140b732c0): vtable, state +0x350, flags +0x3a0/+0x3a1, +0x120 mover
                        for (int k = 0; k < 4; ++k) {
                            const uintptr_t nav = WatchExecRcx(k);
                            if (!nav || !IsReadable(nav, 0x3a8)) continue;
                            Log("follow:   nav[%d] %p result(+0x1fa)=%u points(+0x2f8)=%u +0x322=%u", k, reinterpret_cast<void*>(nav), *reinterpret_cast<uint8_t*>(nav + 0x1fa), *reinterpret_cast<uint16_t*>(nav + 0x2f8), *reinterpret_cast<uint8_t*>(nav + 0x322));
                            Log("follow:   nav[%d] %p vt=%p state(+0x350)=%d +0x354=%d flags=%02x %02x +0x120=%p +0x178=%u req(+0x90)=(%.2f, %.2f, %.2f)",
                                k, reinterpret_cast<void*>(nav), reinterpret_cast<void*>(ReadPtr(nav)),
                                *reinterpret_cast<int32_t*>(nav + 0x350), *reinterpret_cast<int32_t*>(nav + 0x354),
                                *reinterpret_cast<uint8_t*>(nav + 0x3a0), *reinterpret_cast<uint8_t*>(nav + 0x3a1),
                                reinterpret_cast<void*>(ReadPtr(nav + 0x120)), *reinterpret_cast<uint8_t*>(nav + 0x178),
                                *reinterpret_cast<float*>(nav + 0x90), *reinterpret_cast<float*>(nav + 0x94),
                                *reinterpret_cast<float*>(nav + 0x98));
                        }
                    }
                    LogAiState("follow", a);
                    Log("follow: puppet %zu at (%.2f, %.2f, %.2f) -> go to (%.2f, %.2f, %.2f) d=%.2f style %d speed %.2f %s", i,
                        cur.x, cur.y, cur.z, target.x, target.y, target.z, d, style, speed, ok ? "ok" : "NO BRAIN");
                }
            }
        }
    }
    RunOnMainThread(FollowTick);
}

void PuppetsComeToPlayer() {
    g_follow = !g_follow;
    if (g_follow) g_mirror = false;
    Log("puppets: follow (AI go-to) %s (%zu puppets)", g_follow ? "on" : "off", g_puppets.size());
    for (uint32_t goh : g_puppets) LogAiState("puppet", ActorFromGohHandle(goh));
    for (auto& s : g_followState) s = FollowState{};
    if (g_follow) {
        RunOnMainThread(FollowTick);
        // Debug: does the AI brain update (FUN_140b111a0, consumes brain+0x159c "new task" flags) run at all?
        RunOnMainThread([] {
            for (uint32_t goh : g_puppets) LogAiState("puppet(after task)", ActorFromGohHandle(goh));
            // brainUpdate FUN_140b111a0 verified: runs ~2x/frame per puppet brain. Now: does the go-to task update run?
            // gotoUpdate FUN_140bd4ac0 verified: runs every frame per puppet. Now: path request start?
            ArmExecWatch(GameAddr(0x140B732C0), "navRequest");  // capture the nav object (rcx), then write-watch its state
        });
    }
}

// ---- API for the network client (main thread only) ----
bool GetLocalPlayerState(Vec3& pos, float& heading) {
    if (!g_playerActorHandle) return false;
    uintptr_t actor = 0;
    if (!PlayerActorAndPos(actor, pos)) return false;
    heading = ActorHeadingDeg(actor);
    return true;
}

bool GetLocalPlayerHealth(float& health) {
    if (!g_playerActorHandle) return false;
    uintptr_t actor = 0;
    Vec3 p{};
    if (!PlayerActorAndPos(actor, p)) return false;
    const uintptr_t hc = ReadPtr(actor + 0x60);
    if (!hc || !IsReadable(hc + 0x20, 4)) return false;
    health = *reinterpret_cast<float*>(hc + 0x20);
    return true;
}

bool TeleportLocalPlayer(const Vec3& pos, float heading) {
    if (!g_playerActorHandle) return false;
    TeleportActor(g_playerActorHandle, pos, heading);
    return true;
}

uint32_t SpawnRemotePuppet(const Vec3& pos, float heading, const char* label) {
    static uint32_t layout = 0;
    if (!layout) layout = g_puppetOwner ? g_puppetOwner : FindLayoutHandle();
    if (!layout) return 0;
    // Actor names must be unique and stay valid for the actor's lifetime.
    static char names[128][40];
    static int next = 0;
    char* name = names[next % 128];
    sprintf_s(names[next % 128], "rdmp_net_%d", next + 1);
    ++next;
    const Vec3 rot{0.0f, heading, 0.0f};
    const uint32_t h = CreateActorDirect(layout, reinterpret_cast<uint64_t>(name), g_puppetDef, pos, rot, g_puppetVariation < 0 ? 0 : g_puppetVariation);
    Log("net puppet: \"%s\" (%s) def %u at (%.2f, %.2f, %.2f) -> goh 0x%08x", name, label ? label : "", g_puppetDef, pos.x,
        pos.y, pos.z, h);
    if (h) {
        TeleportActor(ActorHandleFromGohHandle(h), pos, heading);
        ApplyStoryComponentMask(ActorFromGohHandle(h), "net puppet");
        const uintptr_t gt = ReadPtr(GameAddr(kGohTablePtr));
        const uintptr_t gobj = gt ? ReadPtr(gt + (h & 0xFFFF) * 0x10) : 0;
        const uintptr_t act = ActorFromGohHandle(h);
        Log("net puppet: goh obj %p vt %p | actor %p vt %p", reinterpret_cast<void*>(gobj),
            reinterpret_cast<void*>(gobj ? ReadPtr(gobj) : 0), reinterpret_cast<void*>(act),
            reinterpret_cast<void*>(act ? ReadPtr(act) : 0));
    }
    return h;
}

bool SetPuppetTransform(uint32_t goh, const Vec3& pos, float heading) {
    const uint32_t ah = ActorHandleFromGohHandle(goh);
    if (!ah) return false;
    TeleportActor(ah, pos, heading);
    return true;
}

// Same transform update without the "warp" branch of FUN_140ad4680 (param_4 = 0): that branch resets the motion
// component *(actor+0x38) (FUN_140ce4190), which is what stops the walk animation on a teleport. Param_4 = 0 calls
// FUN_140cf01f0(motion, 1) instead. For small per-frame position corrections while the puppet locomotes.
bool NudgePuppet(uint32_t goh, const Vec3& pos, float heading) {
    // RE (decomp/setmatrix.c, motion_reset.c, setxform_msg.c): both branches of FUN_140ad4680 reset the motion component
    // *(actor+0x38) (param_4 1: FUN_140ce4190 resets 23 sub-objects; 0: FUN_140cf01f0 + FUN_140cfb6e0 reinit the motion
    // state), which is what stops the locomotion. The move itself is message 7 handled by FUN_140ae0100(entity, msg):
    //   +0x00 u32 7 | +0x10.. old transform rows (3 x vec3 at 16-byte stride + translation) | +0x50 new matrix* |
    //   +0x58 flags (teleport 0x6b; bit0 = reset motion fields + FUN_140cf8310, bit2 = skip the cloth re-init event,
    //   bit6 = move the physics instance via FUN_14089dc60, bit7 = camera snap) | +0x59 bit0 = update the AI position.
    // Sent here directly with bit0 cleared and bit2 set (0x6e): position + facing move, locomotion untouched.
    const uint32_t ah = ActorHandleFromGohHandle(goh);
    if (!ah) return false;
    const uintptr_t table = ReadPtr(GameAddr(0x142C457F8));
    if (!table) return false;
    const uintptr_t slot = table + (ah & 0xFFFF) * 0x10;
    if (*reinterpret_cast<const uint16_t*>(slot + 8) != static_cast<uint16_t>(ah >> 16)) return false;
    const uintptr_t entity = ReadPtr(slot);
    const uintptr_t node = entity ? ReadPtr(entity + 0xb0) : 0;
    const uintptr_t cur = node ? ReadPtr(node + 0x18) : 0;
    if (!cur || !IsReadable(cur, 0x40)) return false;
    const float c = std::cos(heading * 0.01745329f), s = std::sin(heading * 0.01745329f);
    alignas(16) float m[16] = {c, 0.0f, -s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, 0.0f, c, 0.0f, pos.x, pos.y, pos.z, 1.0f};
    alignas(16) uint8_t msg[0x60] = {};
    *reinterpret_cast<uint32_t*>(msg) = 7;
    const float* old = reinterpret_cast<const float*>(cur);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 3; ++col) reinterpret_cast<float*>(msg + 0x10 + row * 0x10)[col] = old[row * 4 + col];
    *reinterpret_cast<float**>(msg + 0x50) = m;
    msg[0x58] = 0x6e;
    msg[0x59] = 1;
    using SendFn = void (*)(uintptr_t entity, void* msg);
    reinterpret_cast<SendFn>(GameAddr(0x140AE0100))(entity, msg);
    return true;
}

// Returns the puppet's actor, giving it an AI brain first if it has none (the locomotion runs inside the per-actor brain
// update; template_player puppets are created without one). 0 = puppet gone.
uintptr_t PreparePuppetLocomotion(uint32_t goh) {
    const uintptr_t actor = ActorFromGohHandle(goh);
    if (!actor) return 0;
    if (!ReadPtr(actor + 0x28)) {
        Vec3 pos{};
        const uintptr_t xf = ReadPtr(actor + 0xc8);
        if (xf && IsReadable(xf + 0x70, 12)) pos = *reinterpret_cast<Vec3*>(xf + 0x70);
        GiveGoToCoord(actor, pos, 1);  // FUN_140af93c0 creates the AI components on demand
        Log("net puppet: goh 0x%08x brain created -> %p", goh, reinterpret_cast<void*>(ReadPtr(actor + 0x28)));
    }
    return actor;
}

bool GetPuppetPose(uint32_t goh, Vec3& pos, float& headingDeg) {
    const uintptr_t actor = ActorFromGohHandle(goh);
    const uintptr_t xf = actor ? ReadPtr(actor + 0xc8) : 0;
    if (!xf || !IsReadable(xf + 0x70, 12)) return false;
    pos = *reinterpret_cast<Vec3*>(xf + 0x70);
    headingDeg = ActorHeadingDeg(actor);
    return true;
}

bool GetPuppetPosition(uint32_t goh, Vec3& pos) {
    const uintptr_t actor = ActorFromGohHandle(goh);
    const uintptr_t xf = actor ? ReadPtr(actor + 0xc8) : 0;
    if (!xf || !IsReadable(xf + 0x70, 12)) return false;
    pos = *reinterpret_cast<Vec3*>(xf + 0x70);
    return true;
}

// Debug (rdmp.ini [debug] collision_probe=1): moves the player toward -X at ~8 m/s (16 m every 2 s, like a galloping
// horse), placed 6 m above the last ground; if he stays frozen, retries the same spot 20 m higher (rising terrain).
// Logs landed / FROZEN / FELL THROUGH and the stream grid -> does collision + map stream in around a moving player?
void CollisionProbeTick() {
    static int step = -1, retries = 0;
    static ULONGLONG next = 0;
    static Vec3 placed{};
    static float lastGround = 0.0f;
    const ULONGLONG now = GetTickCount64();
    if (now >= next) {
        uintptr_t actor = 0;
        Vec3 p{};
        static ULONGLONG validSince = 0;
        const bool valid = PlayerActorAndPos(actor, p) && !(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f);
        if (step < 0 && valid && !validSince) validSince = now;
        if (valid && step < 0 && now - validSince < 5000) {  // let the spawned player settle first
            next = now + 500;
            RunOnMainThread(CollisionProbeTick);
            return;
        }
        if (valid) {
            bool advance = true;
            if (step >= 0) {
                const float dy = p.y - placed.y;
                const char* what = dy < -0.5f ? (dy < -60.0f ? "FELL THROUGH" : "landed") : "FROZEN";
                if (dy >= -0.5f && retries < 3) {
                    ++retries;
                    advance = false;
                    placed.y += 20.0f;
                    Log("probe: step %d at (%.1f, %.1f) FROZEN -> retry %d at y %.1f", step, placed.x, placed.z, retries, placed.y);
                    TeleportActor(g_playerActorHandle, placed, 90.0f);
                } else {
                    if (dy < -0.5f) lastGround = p.y;
                    if (step % 8 == 0 || dy >= -0.5f || dy < -60.0f) {
                        Log("probe: step %d (%.1f, %.1f, %.1f) %s ground %.1f", step, p.x, p.y, p.z, what, lastGround);
                        LogStreamGrid(p);
                    }
                }
            } else {
                lastGround = p.y;
            }
            if (advance) {
                retries = 0;
                ++step;
                if (step > 120) return;  // ~1.9 km
                placed = Vec3{g_spawnPos.x - 16.0f * static_cast<float>(step + 1), lastGround + 6.0f, g_spawnPos.z};
                TeleportActor(g_playerActorHandle, placed, 90.0f);  // face -X (camera follows behind)
            }
        }
        next = now + 2000;
    }
    RunOnMainThread(CollisionProbeTick);
}

// Debug: state diff of the player actor (+0x800) and every object it points to (actor+0..0x400, 0x1000 each).
// TakeStateSnap(i) stores snapshot i (0..3); LogStateDiff() logs u32 fields equal in snaps 0/1 (alive) and in 2/3
// (dead) but different between the two pairs.
struct StateSnap {
    uintptr_t base[0x81];
    uint8_t data[0x81][0x1000];
};
static StateSnap* g_stateSnaps[4];

void TakeStateSnap(int i, uintptr_t actor) {
    if (!g_stateSnaps[i]) g_stateSnaps[i] = new StateSnap{};
    StateSnap& s = *g_stateSnaps[i];
    for (int k = 0; k <= 0x80; ++k) {
        const uintptr_t b = k == 0 ? actor : ReadPtr(actor + (k - 1) * 8);
        const size_t len = k == 0 ? 0x800 : 0x1000;
        s.base[k] = 0;
        if (b < 0x10000 || (b & 7) || !IsReadable(b, len)) continue;
        s.base[k] = b;
        std::memcpy(s.data[k], reinterpret_cast<void*>(b), len);
    }
    Log("statediff: snapshot %d of actor %p", i, reinterpret_cast<void*>(actor));
}

void LogStateDiff() {
    int hits = 0;
    for (int k = 0; k <= 0x80 && hits < 200; ++k) {
        const uintptr_t b = g_stateSnaps[0]->base[k];
        if (!b) continue;
        bool same = true;
        for (int i = 1; i < 4; ++i) same = same && g_stateSnaps[i]->base[k] == b;
        if (!same) continue;
        const size_t len = k == 0 ? 0x800 : 0x1000;
        for (size_t off = 0; off + 4 <= len && hits < 200; off += 4) {
            uint32_t v[4];
            for (int i = 0; i < 4; ++i) std::memcpy(&v[i], g_stateSnaps[i]->data[k] + off, 4);
            if (v[0] != v[1] || v[2] != v[3] || v[0] == v[2]) continue;
            float fa, fd;
            std::memcpy(&fa, &v[0], 4);
            std::memcpy(&fd, &v[2], 4);
            Log("statediff: %s+0x%x +0x%03llx: alive 0x%08x (%g) -> dead 0x%08x (%g)", k == 0 ? "actor" : "[actor", k == 0 ? 0 : (k - 1) * 8,
                static_cast<unsigned long long>(off), v[0], fa, v[2], fd);
            ++hits;
        }
    }
    Log("statediff: %d field(s)", hits);
}

// Debug (collision_probe=2): once settled, put the player at (-250, 113, 1199.33) facing -X (just before the spot where
// the camera broke during the teleport probe) so a held W walks him across it; StreamingFrame logs the focus.
void WalkProbeTick() {
    static ULONGLONG validSince = 0;
    const ULONGLONG now = GetTickCount64();
    uintptr_t actor = 0;
    Vec3 p{};
    const bool valid = PlayerActorAndPos(actor, p) && !(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f);
    if (valid && !validSince) validSince = now;
    if (!valid || now - validSince < 5000) {
        RunOnMainThread(WalkProbeTick);
        return;
    }
    TeleportActor(g_playerActorHandle, Vec3{-250.0f, 113.0f, 1199.33f}, 90.0f);
    CameraToPlayer("walk probe");
    Log("probe: walk probe start at (-250, 113, 1199.33) facing -X");
}

// Remote-player stream out / in. The puppet's game object (goh, class Actor, vt 0x141C82B90) stays in the goh table;
// its actor instance is created / released by the object's own lifecycle methods:
//   Actor::vf0 (0x140264ed0) = activate: model resources (+0xc0) + FUN_140264b80 -> actor instance (actor table)
//   Actor::vf2 (0x140265040) = deactivate: releases the actor instance (FUN_140ae9b90) and the object's pool entry
// Main thread only.
uintptr_t GohObject(uint32_t goh) {
    const uintptr_t table = ReadPtr(GameAddr(kGohTablePtr));
    const uintptr_t e = table ? table + (goh & 0xFFFF) * 0x10 : 0;
    if (!e || !IsReadable(e, 0x10) || *reinterpret_cast<uint16_t*>(e + 8) != static_cast<uint16_t>(goh >> 16)) return 0;
    return ReadPtr(e);
}

bool StreamOutPuppet(uint32_t goh) {
    const uintptr_t obj = GohObject(goh);
    if (!obj) return false;
    const uintptr_t actor = ActorFromGohHandle(goh);
    if (!actor) return true;  // already out
    ClearPuppetDesire(actor);
    // vf2 releases the model/visuals but the actor instance survives (its blob shadow stayed on the ground in tests):
    // park it far below the map first; StreamInPuppet teleports it back.
    {
        const uintptr_t xf = ReadPtr(actor + 0xc8);
        if (xf && IsReadable(xf + 0x70, 12)) {
            Vec3 p = *reinterpret_cast<Vec3*>(xf + 0x70);
            p.y -= 500.0f;
            TeleportActor(ActorHandleFromGohHandle(goh), p, 0.0f);
        }
    }
    using Fn = void (*)(uintptr_t);
    reinterpret_cast<Fn>(ReadPtr(ReadPtr(obj) + 0x10))(obj);  // vf2: deactivate
    Log("stream: goh 0x%08x streamed out (actor %p released; actor now %p)", goh, reinterpret_cast<void*>(actor),
        reinterpret_cast<void*>(ActorFromGohHandle(goh)));
    return true;
}

bool StreamInPuppet(uint32_t goh, const Vec3& pos, float heading) {
    const uintptr_t obj = GohObject(goh);
    if (!obj) return false;
    if (!ActorFromGohHandle(goh)) {
        using Fn = void (*)(uintptr_t);
        reinterpret_cast<Fn>(ReadPtr(ReadPtr(obj)))(obj);  // vf0: activate
    }
    const uintptr_t actor = ActorFromGohHandle(goh);
    if (!actor) {
        Log("stream: goh 0x%08x activation produced no actor", goh);
        return false;
    }
    TeleportActor(ActorHandleFromGohHandle(goh), pos, heading);
    ApplyStoryComponentMask(actor, "streamed-in puppet");
    Log("stream: goh 0x%08x streamed in -> actor %p at (%.1f, %.1f, %.1f)", goh, reinterpret_cast<void*>(actor), pos.x, pos.y, pos.z);
    return true;
}

// Loader fader (FUN_14063af00): loader+0xc418 fader movie, +0x257e5 bit 8 fade active, +0x257ec bit 1 = towards black,
// +0x25800 value (1 = black), +0x25804 alpha scale, +0x25808 speed, +0x257f0..f8 color.
void LogFader(const char* tag) {
    const uintptr_t L = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
    if (!L) return;
    auto f = [L](uintptr_t o) { return *reinterpret_cast<float*>(L + o); };
    auto b = [L](uintptr_t o) { return *reinterpret_cast<uint8_t*>(L + o); };
    Log("fader[%s]: movie=%p e5=%02x e6=%02x ec=%02x val=%.3f alpha=%.3f speed=%.3f color=(%.3f %.3f %.3f)", tag,
        *reinterpret_cast<void**>(L + 0xc418), b(0x257e5), b(0x257e6), b(0x257ec), f(0x25800), f(0x25804), f(0x25808),
        f(0x257f0), f(0x257f4), f(0x257f8));
}

// Debug ([debug] pos_log=1): local player position + heading every 3 s (works without the sandbox spawn: falls back
// to playerInfo+0x5ec).
void PosLogFrame(float) {
    static ULONGLONG last = 0;
    if (GetTickCount64() - last < 3000) return;
    last = GetTickCount64();
    uintptr_t actor = 0;
    Vec3 p{};
    if (!PlayerActorAndPos(actor, p)) return;
    const uintptr_t info = ReadPtr(GameAddr(kPlayerInfoPtr));
    const uint8_t* c = actor ? reinterpret_cast<const uint8_t*>(ReadPtr(actor + 0x70)) : nullptr;
    {
        const uintptr_t cm = ReadPtr(GameAddr(kCameraMgrPtr));
        const uintptr_t cam = cm ? ReadPtr(cm + 8 + static_cast<int64_t>(*reinterpret_cast<int32_t*>(cm + 0x48)) * 8) : 0;
        if (cam && IsReadable(cam + 0x200, 12)) { const float* c = reinterpret_cast<const float*>(cam + 0x200); const float* fw = reinterpret_cast<const float*>(cam + 0x1f0); Log("pos: camera (%.2f, %.2f, %.2f) fwd (%.2f, %.2f, %.2f)", c[0], c[1], c[2], fw[0], fw[1], fw[2]); }
    }
    Log("pos: player (%.2f, %.2f, %.2f) heading %.1f handle 0x%08x controls 0x%02x", p.x, p.y, p.z, ActorHeadingDeg(actor),
        info ? *reinterpret_cast<uint32_t*>(info + 0x5EC) : 0, c && IsReadable(reinterpret_cast<uintptr_t>(c) + 0x48e, 1) ? c[0x48e] : 0xEE);
}

void FaderDebugFrame(float) {
    static ULONGLONG last = 0;
    static int n = 0;
    const ULONGLONG now = GetTickCount64();
    if (n > 400 || now - last < 250) return;
    last = now;
    ++n;
    LogFader("tick");
}

// Engine screen fade (the loader's fader + "FadeToBlack" UI scene, as used by the loading screens and the scripted
// TransitionEventHudFadeOut/In): FUN_14063b1b0(loader, seconds, alpha, flag) fades to black, FUN_14063b0a0(loader,
// seconds, alpha, flag) fades back in (+ "loadComplete"); the frame update calls the latter on finish-loading with
// the args at loader+0x25814 (seconds) / +0x25818 (alpha). Main thread only.
constexpr uintptr_t kFadeToBlack = 0x14063B1B0;
void EngineFadeToBlack(float seconds, const char* why) {
    const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
    if (!loader) return;
    reinterpret_cast<void (*)(uintptr_t, float, float, uint8_t)>(GameAddr(kFadeToBlack))(loader, seconds, 1.0f, 0);
    {  // fader colour (FUN_14063b3c0: rgb + alpha); ini fade_color_test=1 uses red to see where the fader shows
        const bool test = GetPrivateProfileIntW(L"debug", L"fade_color_test", 0, g_iniPath) != 0;
        float rgba[4] = {test ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f};
        reinterpret_cast<void (*)(uintptr_t, float*)>(GameAddr(0x14063B3C0))(loader, rgba);
    }
    Log("fade: to black (%.2f s) at %s", seconds, why);
}

// Releases the held engine fade-in (see RequestFinishLoading) once the player and camera have settled.
void FadeHoldFrame(float) {
    if (!g_fadeInAt || GetTickCount64() < g_fadeInAt) return;
    g_fadeInAt = 0;
    const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
    if (!loader) return;
    reinterpret_cast<void (*)(uintptr_t, float, float, uint8_t)>(GameAddr(0x14063B0A0))(loader, g_fadeInSec, 1.0f, 0);
    Log("fade: into the world (%.2f s)", g_fadeInSec);
}

// Debug ([debug] signin_log=1): logs when the game's own "signed in" check (FUN_140ef0e70(*(0x142AAC8D8)), used by
// FUN_140330a60 before showing "SocialClubReturnToMenu") changes.
void SignInProbe(float) {
    static int last = -1;
    const uintptr_t plat = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8D8));
    if (!plat) return;
    const bool in = reinterpret_cast<bool (*)(uintptr_t)>(GameAddr(0x140EF0E70))(plat);
    if (static_cast<int>(in) != last) {
        last = in;
        Log("signin: game check -> %d", in);
    }
}

void RequestFinishLoading(const char* why) {
    const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
    if (!loader) {
        Log("finish loading (%s): loader object is null", why);
        return;
    }
    if (g_engineFade) {  // FinishLoading fades the world in from black over g_fadeInSec
        *reinterpret_cast<float*>(loader + 0x25814) = 0.0f;  // instant; SpawnPlayer re-fades to black, FadeHoldFrame fades in
        *reinterpret_cast<float*>(loader + 0x25818) = 1.0f;
    }
    auto* flags = reinterpret_cast<volatile uint8_t*>(loader + kLoaderFlags);
    *flags = static_cast<uint8_t>(*flags | 0x10);
    Log("finish loading requested (%s)", why);
}

// World streaming grid (RE: FUN_140619440 "is the area around pos loaded", called by FUN_1405b3ff0 with
// *(0x142ac5098)): g = *(mgr+0x20) floats {cellX, cellY, cellZ, ..., [0xe] = width (as int)}; cells = *(mgr+8) (ptr per
// cell, row-major x + z*width); origin x/z = *(float*)0x1422e42e0 / 0x1422e42e8, max x/z = 0x1422e42d0 / 0x1422e42d8.
// A cell is loaded when *(u8*)(cell+0xc) != 0.
void LogStreamGrid(const Vec3& p) {
    const uintptr_t mgr = ReadPtr(GameAddr(0x142AC5098));
    if (!mgr || !IsReadable(mgr, 0x28)) { Log("grid: no manager"); return; }
    const uintptr_t gp = ReadPtr(mgr + 0x20), cells = ReadPtr(mgr + 8);
    if (!gp || !IsReadable(gp, 0x40) || !cells) { Log("grid: no grid info"); return; }
    const float* g = reinterpret_cast<const float*>(gp);
    const int width = *reinterpret_cast<const int32_t*>(gp + 0xe * 4);
    const float minX = *reinterpret_cast<float*>(GameAddr(0x1422E42E0)), minZ = *reinterpret_cast<float*>(GameAddr(0x1422E42E8));
    const float maxX = *reinterpret_cast<float*>(GameAddr(0x1422E42D0)), maxZ = *reinterpret_cast<float*>(GameAddr(0x1422E42D8));
    const int height = g[2] > 0 ? static_cast<int>((maxZ - minZ) / g[2]) + 1 : 0;
    int nonNull = 0, loaded = 0;
    for (int i = 0; i < width * height && i < 1000000; ++i) {
        if (!IsReadable(cells + i * 8, 8)) break;
        const uintptr_t c = ReadPtr(cells + i * 8);
        if (!c) continue;
        ++nonNull;
        if (IsReadable(c + 0xc, 1) && *reinterpret_cast<uint8_t*>(c + 0xc)) ++loaded;
    }
    const int cx = static_cast<int>((p.x - minX) / g[0]), cz = static_cast<int>((p.z - minZ) / g[2]);
    Log("grid: mgr %p cell %.1f x %.1f x %.1f, %d x %d, origin (%.1f, %.1f) max (%.1f, %.1f), %d cells present, %d loaded; player cell (%d, %d)",
        reinterpret_cast<void*>(mgr), g[0], g[1], g[2], width, height, minX, minZ, maxX, maxZ, nonNull, loaded, cx, cz);
    for (int dz = -2; dz <= 2; ++dz) {
        char line[256];
        int n = 0;
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = cx + dx, z = cz + dz;
            const uintptr_t c = (x >= 0 && z >= 0 && x < width && z < height) ? ReadPtr(cells + (x + z * width) * 8) : 0;
            const int st = !c ? -1 : (IsReadable(c + 0xc, 1) ? *reinterpret_cast<uint8_t*>(c + 0xc) : -2);
            n += sprintf_s(line + n, sizeof(line) - n, "%3d ", st);
        }
        Log("grid:   z%+d: %s  (-1 = no cell, else loaded flag)", dz, line);
    }
}

// ---- world streaming focus (collision / navmesh follow this, not the camera) ----
// RE: FUN_1405b3ff0 ("is the world loaded around pos") reads focus points *(*(0x142ac50d0)+0xa0) (count u16 at +0xa8);
// each starts with a world position (x, y, z). Logged every 2 s to see whether the focus follows the player.
constexpr uintptr_t kStreamFocusMgrPtr = 0x142AC50D0;

void StreamingFrame(float) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 30000) return;  // diagnostics every 30 s
    last = now;
    const uintptr_t mgr = ReadPtr(GameAddr(kStreamFocusMgrPtr));
    if (!mgr || !IsReadable(mgr + 0xa0, 0x10)) return;
    const uintptr_t foci = ReadPtr(mgr + 0xa0);
    const uint16_t count = *reinterpret_cast<uint16_t*>(mgr + 0xa8);
    uintptr_t actor = 0;
    Vec3 p{};
    const bool havePlayer = g_playerActorHandle && PlayerActorAndPos(actor, p);
    if (!foci || !IsReadable(foci, 0x30)) {
        Log("stream: mgr %p foci=%p count=%u", reinterpret_cast<void*>(mgr), reinterpret_cast<void*>(foci), count);
        return;
    }
    if (havePlayer) {
        // FUN_1405b3ff0(streamer = *(0x142ac50a0), pos, -, radius): "world loaded around pos" (frame task uses 50 m).
        using LoadedFn = uint64_t (*)(uintptr_t streamer, const float* pos, uint64_t unused, float radius);
        const uintptr_t streamer = ReadPtr(GameAddr(0x142AC50A0));
        const uint8_t r = streamer ? static_cast<uint8_t>(reinterpret_cast<LoadedFn>(GameAddr(0x1405B3FF0))(streamer, &p.x, 0, 50.0f)) : 0xff;
        const uintptr_t s5140 = ReadPtr(GameAddr(0x142AC5140)), s52d8 = ReadPtr(GameAddr(0x142AC52D8));
        const uintptr_t sAaac0 = ReadPtr(GameAddr(0x142ACAAC0)), s1c90 = ReadPtr(GameAddr(0x142AD1C90));
        Log("stream: loaded(player,50m)=%u | 5140+128=%u +129=%u 52d8+80=%u 50d0+1fa4=%u 1c90+2524e/f=%u/%u radius=%.1f/%.1f "
            "streamer+1c794=0x%08x +1c74c=%d",
            r, s5140 ? *reinterpret_cast<uint8_t*>(s5140 + 0x128) : 0xff, s5140 ? *reinterpret_cast<uint8_t*>(s5140 + 0x129) : 0xff,
            s52d8 ? *reinterpret_cast<uint8_t*>(s52d8 + 0x80) : 0xff, *reinterpret_cast<uint8_t*>(mgr + 0x1fa4),
            s1c90 ? *reinterpret_cast<uint8_t*>(s1c90 + 0x2524e) : 0xff, s1c90 ? *reinterpret_cast<uint8_t*>(s1c90 + 0x2524f) : 0xff,
            sAaac0 ? *reinterpret_cast<float*>(sAaac0 + 0x18) : -1.0f, sAaac0 ? *reinterpret_cast<float*>(sAaac0 + 8) : -1.0f,
            streamer ? *reinterpret_cast<uint32_t*>(streamer + 0x1c794) : 0, streamer ? *reinterpret_cast<int32_t*>(streamer + 0x1c74c) : -1);
        static int gridCount = 0;
        LogStreamGrid(p);
    }
    const float* f = reinterpret_cast<const float*>(foci);
    Log("stream: %u focus point(s), [0] = (%.2f, %.2f, %.2f) w=%.2f +0x2c=%u | player %s(%.2f, %.2f, %.2f)", count, f[0], f[1],
        f[2], f[3], *reinterpret_cast<const uint8_t*>(foci + 0x2c), havePlayer ? "" : "(none) ", p.x, p.y, p.z);
}

void SceneReportFrame(float);  // debug, below

void LoadSandboxConfig(const wchar_t* ini) {
    AddFrameCallback(&StreamingFrame);
    AddFrameCallback(&DeathMonitor);
    AddFrameCallback(&LimboTick);
    if (GetPrivateProfileIntW(L"debug", L"fader_log", 0, ini)) AddFrameCallback(&FaderDebugFrame);
    AddFrameCallback(&FadeHoldFrame);
    if (GetPrivateProfileIntW(L"debug", L"pos_log", 0, ini)) AddFrameCallback(&PosLogFrame);
    if (GetPrivateProfileIntW(L"debug", L"scene_report", 0, ini)) AddFrameCallback(&SceneReportFrame);
    if (GetPrivateProfileIntW(L"debug", L"signin_log", 0, ini)) AddFrameCallback(&SignInProbe);
    InstallBootVideoHook(ini);
    InstallGameFlow(ini);
    wcsncpy_s(g_iniPath, ini, _TRUNCATE);
    g_enabled = GetPrivateProfileIntW(L"sandbox", L"enabled", 0, ini) != 0;
    if (g_enabled && GetPrivateProfileIntW(L"sandbox", L"loading_overlay", 1, ini)) StartLoadingOverlay();
    g_autoFinish = GetPrivateProfileIntW(L"sandbox", L"auto_finish_loading", 1, ini) != 0;
    g_spawnPlayer = GetPrivateProfileIntW(L"sandbox", L"spawn_player", 0, ini) != 0; // script-driven by default
    g_limboSpawn = GetPrivateProfileIntW(L"sandbox", L"limbo_spawn", 1, ini) != 0;
    {
        wchar_t v[64] = L"";
        GetPrivateProfileStringW(L"sandbox", L"limbo_pos", L"0 -300 0", v, 64, ini);
        float x = 0.0f, y = -300.0f, z = 0.0f;
        if (swscanf_s(v, L"%f %f %f", &x, &y, &z) == 3) g_limboPos = Vec3{x, y, z};
    }
    g_playerActorType = GetPrivateProfileIntW(L"sandbox", L"player_actor_type", 808, ini);
    g_spawnPos = {IniFloat(ini, L"spawn_x", 736.49f), IniFloat(ini, L"spawn_y", 89.17f),
                  IniFloat(ini, L"spawn_z", 1199.33f)};
    g_spawnHeading = IniFloat(ini, L"spawn_heading", 0.0f);
    if (InstanceId() > 1) {
        // Extra game copies on this PC (RDMP.exe --instance N): spawn 5*(N-1) m in front of the first player, turned
        // around, so the players face each other. Engine forward = (-sin h, 0, -cos h).
        const float h = g_spawnHeading * 0.01745329f, d = 5.0f * static_cast<float>(InstanceId() - 1);
        g_spawnPos.x += -std::sin(h) * d;
        g_spawnPos.z += -std::cos(h) * d;
        g_spawnHeading += g_spawnHeading > 0.0f ? -180.0f : 180.0f;
        Log("sandbox: instance %d spawns %.0f m ahead of the first player, heading %.1f", InstanceId(), d, g_spawnHeading);
    }
    g_finishDelayMs = GetPrivateProfileIntW(L"sandbox", L"finish_delay_ms", 8000, ini);
    g_ownerHandle = GetPrivateProfileIntW(L"sandbox", L"owner_handle", 808, ini);
    g_skipMenusMode = GetPrivateProfileIntW(L"sandbox", L"skip_menus", 1, ini);
    g_skipMenus = g_skipMenusMode != 0;
    {
        wchar_t ev[64];
        GetPrivateProfileStringW(L"sandbox", L"direct_start_event", L"fileDoFreeSpaceCheck", ev, 64, ini);
        WideCharToMultiByte(CP_UTF8, 0, ev, -1, g_directStartEvent, sizeof(g_directStartEvent), nullptr, nullptr);
    }
    g_killScripts = GetPrivateProfileIntW(L"sandbox", L"kill_scripts", 1, ini) != 0;
    g_playerVariation = static_cast<int32_t>(GetPrivateProfileIntW(L"sandbox", L"player_variation", 12, ini));
    g_puppetVariation = static_cast<int32_t>(GetPrivateProfileIntW(L"sandbox", L"puppet_variation", 12, ini));
    g_clearScriptCamera = GetPrivateProfileIntW(L"sandbox", L"clear_script_camera", 1, ini) != 0;
    g_puppetDef = GetPrivateProfileIntW(L"sandbox", L"puppet_def", 0, ini);
    g_followStyle = GetPrivateProfileIntW(L"sandbox", L"follow_style", 0, ini);
    g_respawnDelayMs = GetPrivateProfileIntW(L"sandbox", L"respawn_delay_ms", 5000, ini);
    g_engineFade = GetPrivateProfileIntW(L"sandbox", L"engine_fade", 1, ini) != 0;
    g_fadeInSec = GetPrivateProfileIntW(L"sandbox", L"fade_in_ms", 1500, ini) / 1000.0f;
    g_fadeHoldMs = GetPrivateProfileIntW(L"sandbox", L"fade_hold_ms", 1000, ini);
    g_puppetsAtSpawn = GetPrivateProfileIntW(L"sandbox", L"puppets_at_spawn", 0, ini);
    if (g_enabled) {
        Log("sandbox: enabled (auto_finish_loading=%d spawn_player=%d type=%u pos=(%.2f, %.2f, %.2f))", g_autoFinish,
            g_spawnPlayer, g_playerActorType, g_spawnPos.x, g_spawnPos.y, g_spawnPos.z);
    }
}

void SandboxOnWorldLoadStarted() {
    if (!g_enabled || !g_inSinglePlayer) return;
    g_worldLoading = true;
    g_worldLoadStartMs = GetTickCount64();
    Log("sandbox: world load started (finishing the loading screen after %u ms)", g_finishDelayMs);
}

// Menu skipping. Posting the menu's UI events does nothing (they are notifications; the Play flow runs in the
// menu's ActionScript), so we press the keys a player would: Enter on the title screen ("Play").
struct KeyTap { WORD scan; DWORD delayMs; const char* why; };

bool GameIsForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI KeyTapThread(LPVOID p) {
    KeyTap tap = *static_cast<KeyTap*>(p);
    delete static_cast<KeyTap*>(p);
    Sleep(tap.delayMs);
    if (!GameIsForeground()) {
        Log("sandbox: %s: game window not focused, not pressing the key", tap.why);
        return 0;
    }
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = tap.scan;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(80);
    SendInput(1, &in[1], sizeof(INPUT));
    Log("sandbox: %s: pressed scan 0x%02x", tap.why, tap.scan);
    return 0;
}

void TapKeyLater(WORD scan, DWORD delayMs, const char* why) {
    HANDLE t = CreateThread(nullptr, 0, &KeyTapThread, new KeyTap{scan, delayMs, why}, 0, nullptr);
    if (t) CloseHandle(t);
}

constexpr WORD kScanEnter = 0x1C;

// Title -> mode select ("Normal" preselected) -> load: keep pressing Enter until the game posts
// "enterSinglePlayer". The menu ignores input for a moment after each screen appears, hence the retries.
std::atomic<bool> g_enteredSinglePlayer{false};

DWORD WINAPI PressPlayThread(LPVOID) {
    Sleep(2000);
    for (int i = 0; i < 10 && !g_enteredSinglePlayer; ++i) {
        if (GameIsForeground()) {
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wScan = in[1].ki.wScan = kScanEnter;
            in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &in[0], sizeof(INPUT));
            Sleep(80);
            SendInput(1, &in[1], sizeof(INPUT));
            Log("sandbox: menu: pressed Enter (%d)", i + 1);
        } else {
            Log("sandbox: menu: game window not focused, waiting");
        }
        Sleep(1500);
    }
    if (!g_enteredSinglePlayer) Log("sandbox: menu: gave up pressing Play");
    return 0;
}

// Direct start (skip_menus=2): what the mode-select "Normal" choice does (ActionScript callback ValidateNormalMode,
// FUN_14032f970) minus the UI bits: FUN_14063a900(loader) = show the loading screen and push the game-load state
// machines (state stack *(0x142c45610)). Called on an early UI event, it skips the legal screen, title and menus.
constexpr uintptr_t kStartGameLoad = 0x14063A900;
constexpr uintptr_t kGamePtr = 0x142C456D0;  // game object (+0x12e flags: 1 new game, 4/8 load kinds, 0x10 sp_idle)


uint32_t SandboxPlayerGoh() { return g_playerGoh; }
uint32_t LocalPlayerActorHandle() { return g_playerActorHandle; }

void SandboxMarkInGame() {
    g_enteredSinglePlayer = true;
    g_inSinglePlayer = true;
}

void StartGameDirect() {
    const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
    if (!loader) {
        Log("sandbox: direct start: no loader yet");
        return;
    }
    // The per-frame level-load trigger FUN_1401f38d0(game) starts a level load ("loadStart" -> FUN_1401f85b0 ->
    // world load -> root script) when game+0x12e bit 0 ("new game") is set and the loading screen is up. That skips the
    // menus AND the save/profile chain (fileStartPlayerChecks ... fileSaveSuccess/fileNewSaveCreated ... fileLoad*).
    const uintptr_t game = *reinterpret_cast<uintptr_t*>(GameAddr(kGamePtr));
    Log("sandbox: direct start: loading screen (FUN_14063a900, loader %p) + new-game flag (game %p +0x12e 0x%02x)",
        reinterpret_cast<void*>(loader), reinterpret_cast<void*>(game), game ? *reinterpret_cast<uint8_t*>(game + 0x12e) : 0);
    // Close the title UI first (its 3D background scene kept rendering after the level load -> crash in a shader bind,
    // RDR.exe+0x167315). These notifications only trigger the sign-in check, not the save chain (that comes from the
    // mode-select "Normal" callback, which we skip).
    PostUiEvent("startScreenExit");
    PostUiEvent("mainMenuExit");
    // What the game-flow handler does on "fileStartupChecksComplete" (part of the skipped save chain): DAT_142ac2956 = 1
    // and FUN_140637520(loader) = HUD map init (lodStreamingMap, map shader "rdr2_map"/blit_draw, map blips). Without it
    // the minimap's first draw crashes (null shader, RDR.exe+0x167315).
    *reinterpret_cast<uint8_t*>(GameAddr(0x142AC2956)) = 1;
    reinterpret_cast<void (*)(uintptr_t)>(GameAddr(0x140637520))(loader);
    reinterpret_cast<void (*)(uintptr_t)>(GameAddr(kStartGameLoad))(loader);
    if (game) *reinterpret_cast<uint8_t*>(game + 0x12e) |= 1;
    g_enteredSinglePlayer = true;
    g_inSinglePlayer = true;
}

// UI scenes by name (RE: game-flow code): uiMgr = *(0x142aac8b8); root = *(uiMgr+0xa0); scene = FUN_1400ef100(root, name);
// scene->vf[0xd8/8](scene, 1) = is active; vf[0x158/8](scene) = hide (used on "LoadList"/"SavePromptStrip").
constexpr uintptr_t kUiMgrPtr = 0x142AAC8B8;
constexpr uintptr_t kUiFindScene = 0x1400EF100;

uintptr_t FindUiScene(const char* name) {
    const uintptr_t mgr = ReadPtr(GameAddr(kUiMgrPtr));
    const uintptr_t root = mgr ? ReadPtr(mgr + 0xa0) : 0;
    if (!root) return 0;
    return reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(kUiFindScene))(root, name);
}

bool UiSceneActive(uintptr_t scene) {
    return scene && reinterpret_cast<bool (*)(uintptr_t, int)>(ReadPtr(ReadPtr(scene) + 0xd8))(scene, 1);
}

void LogActiveBootScenes(const char* ev) {
    static const char* kNames[] = {"StartScreen1", "StartScreen2", "StartScreenLegals", "StartScreen", "IntroCredits", "FadeToBlack", "LoadingSpinner", "HudScene", "HudSceneOffline", "HudSceneLayer", "PauseMenuScene", "DeathScreen", "Cutscenes", "OptionsLayer", "ExtrasLayer", "MapLayer",
                                   "LoadingScreen", "Startup", "Setup", "Movies", "Credits", "UserInterface"};
    char line[512];
    int n = 0;
    for (const char* nm : kNames) {
        const uintptr_t s = FindUiScene(nm);
        if (s && UiSceneActive(s)) n += sprintf_s(line + n, sizeof(line) - n, "%s ", nm);
    }
    line[n] = '\0';
    Log("ui scenes active at \"%s\": %s", ev, n ? line : "(none of the candidates)");
}

// Debug ([debug] scene_report=1): active UI scenes 2 s and 6 s after the player spawned.
void SceneReportFrame(float) {
    static int done = 0;
    static ULONGLONG spawnSeen = 0;
    if (!g_playerSpawned || done >= 2) return;
    if (!spawnSeen) spawnSeen = GetTickCount64();
    if (GetTickCount64() - spawnSeen >= (done == 0 ? 2000u : 6000u)) {
        ++done;
        LogActiveBootScenes(done == 1 ? "spawn+2s" : "spawn+6s");
        {
            const uintptr_t game = ReadPtr(GameAddr(0x142C456D0));
            const uintptr_t L = ReadPtr(GameAddr(0x142C456C8));
            const uintptr_t rm = ReadPtr(GameAddr(0x142AC50D0));
            const uintptr_t ui = ReadPtr(GameAddr(0x142AAC8B8));
            auto b = [](uintptr_t p, uintptr_t o) { return p ? *reinterpret_cast<uint8_t*>(p + o) : 0xEE; };
            Log("  state: bd0=%d c0c=%d 2927=%d 2954=%d 2956=%d game+128..12f=%02x %02x %02x %02x %02x %02x %02x %02x rm34=%d ui78=%d ui7c=%d ui175=%d L.e5=%02x L.ec=%02x L.c350=%d",
                *reinterpret_cast<int*>(GameAddr(0x1422D9BD0)), *reinterpret_cast<int*>(GameAddr(0x1422D9C0C)), b(GameAddr(0x142AC2927), 0),
                b(GameAddr(0x142AC2954), 0), b(GameAddr(0x142AC2956), 0), b(game, 0x128), b(game, 0x129), b(game, 0x12a), b(game, 0x12b),
                b(game, 0x12c), b(game, 0x12d), b(game, 0x12e), b(game, 0x12f), b(rm, 0x34), b(ui, 0x78), b(ui, 0x7c), b(ui, 0x175),
                b(L, 0x257e5), b(L, 0x257ec), b(L, 0xc350));
            Log("  time: dt=%.4f clock=%.3f rmFrames(+0x28)=%d", *reinterpret_cast<float*>(GameAddr(0x142314D08)), *reinterpret_cast<float*>(GameAddr(0x142314D3C)), rm ? *reinterpret_cast<int*>(rm + 0x28) : -1);
            if (rm) { const uintptr_t foci = ReadPtr(rm + 0xa0); const float* ff = foci ? reinterpret_cast<const float*>(foci) : nullptr; Log("  stream focus: count=%u [0]=(%.1f, %.1f, %.1f)", *reinterpret_cast<uint16_t*>(rm + 0xa8), ff ? ff[0] : 0.f, ff ? ff[1] : 0.f, ff ? ff[2] : 0.f); }
            Log("  loader+257c0..cf: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", b(L,0x257c0), b(L,0x257c1), b(L,0x257c2), b(L,0x257c3), b(L,0x257c4), b(L,0x257c5), b(L,0x257c6), b(L,0x257c7), b(L,0x257c8), b(L,0x257c9), b(L,0x257ca), b(L,0x257cb), b(L,0x257cc), b(L,0x257cd), b(L,0x257ce), b(L,0x257cf));
        }
        {  // input: active pad index (uiMgr+0x98 -> +0x1c) and the pads' connected flags (FUN_140144d10 -> pad+0x18)
            const uintptr_t uim = ReadPtr(GameAddr(0x142AAC8B8));
            const uintptr_t uin = uim ? ReadPtr(uim + 0x98) : 0;
            const int active = uin ? *reinterpret_cast<int*>(uin + 0x1c) : -9;
            const uintptr_t padsHolder = reinterpret_cast<uintptr_t (*)()>(GameAddr(0x140144D10))();
            const uintptr_t pads = padsHolder ? ReadPtr(padsHolder) : 0;
            char pl[128];
            int n = 0;
            for (int i = 0; i < 6 && pads; ++i) {
                const uintptr_t p = ReadPtr(pads + i * 8);
                const bool valid = p > 0x10000 && p < 0x7FF000000000ULL;
                n += sprintf_s(pl + n, sizeof(pl) - n, " [%d]%s", i, valid ? (*reinterpret_cast<uint8_t*>(p + 0x18) ? "on" : "off") : "-");
            }
            Log("  input: active pad %d, pads:%s", active, n ? pl : " ?");
        }
        {  // player controller "controls disabled" bits
            const uintptr_t table = ReadPtr(GameAddr(kActorHandleTablePtr));
            const uint32_t h = g_playerActorHandle;
            const uintptr_t actor = (table && h) ? ReadPtr(table + (h & 0xFFFF) * 0x10) : 0;
            const uintptr_t comp = actor ? ReadPtr(actor + 0x70) : 0;
            Log("  control: actor %p comp %p flags(+0x48e)=%02x +0x48c..0x490: %02x %02x %02x %02x %02x", reinterpret_cast<void*>(actor),
                reinterpret_cast<void*>(comp), comp ? *reinterpret_cast<uint8_t*>(comp + 0x48e) : 0xEE,
                comp ? *reinterpret_cast<uint8_t*>(comp + 0x48c) : 0, comp ? *reinterpret_cast<uint8_t*>(comp + 0x48d) : 0,
                comp ? *reinterpret_cast<uint8_t*>(comp + 0x48e) : 0, comp ? *reinterpret_cast<uint8_t*>(comp + 0x48f) : 0,
                comp ? *reinterpret_cast<uint8_t*>(comp + 0x490) : 0);
        }
        {  // player locomotion desire (input -> controller): words [0x14] right, [0x16] -forward, [0x3f] flags, + seq
            uint8_t d[kDesireSize] = {};
            uint32_t seq = 0;
            const bool have = GetPlayerDesire(d, &seq);
            const float* f = reinterpret_cast<const float*>(d);
            const uint32_t* u = reinterpret_cast<const uint32_t*>(d);
            Log("  desire: %s seq=%u right=%.2f fwd=%.2f turn=%.2f flags=%08x", have ? "yes" : "no", seq, f[0x14], -f[0x16], f[0x34], u[0x3f]);
        }
        LogStreamGrid(g_spawnPos);
        {  // actor def 0 (template_player): defs = *(0x142B12070) (pointer array), def+0x10 = name
            const uintptr_t defs = ReadPtr(GameAddr(0x142B12070));
            const uintptr_t def0 = defs ? ReadPtr(defs) : 0;
            if (def0 && IsReadable(def0, 0x100)) {
                const uint64_t* q = reinterpret_cast<const uint64_t*>(def0);
                Log("  def0 %p name=%s", reinterpret_cast<void*>(def0), IsReadable(q[2], 16) ? reinterpret_cast<const char*>(q[2]) : "?");
                for (int r = 0; r < 32; r += 4)
                    Log("  def0+%03x: %016llx %016llx %016llx %016llx", r * 8, q[r], q[r + 1], q[r + 2], q[r + 3]);
            }
        }
        {  // radar map: flags + tile slots (radar = loader+0xc5e0, slots +0x138 stride 0x18: +8 id, +0xc flags)
            const uintptr_t L = ReadPtr(GameAddr(0x142C456C8));
            const uintptr_t radar = L ? L + 0xc5e0 : 0;
            const int n = *reinterpret_cast<int*>(GameAddr(0x142BEE95C));
            int valid = 0, loaded = 0, other = 0;
            for (int i = 0; radar && i < n * n && i < 0x90; ++i) {
                const uintptr_t s = radar + 0x138 + i * 0x18;
                const int id = *reinterpret_cast<int*>(s + 8);
                const uint32_t fl = *reinterpret_cast<uint32_t*>(s + 0xc);
                if (id != -1) ++valid;
                if ((fl & 0x30000020) == 0x30000000) ++loaded; else if (fl) ++other;
            }
            { const uintptr_t hd = FindUiScene("HudDefault"); Log("  radar render gate: d32d=%d 09e7=%d L.257c4=%d L.257c0=%d HudDefault=%p active=%d L.2581f=%d a81a=%d", *reinterpret_cast<uint8_t*>(GameAddr(0x14227D32D)), *reinterpret_cast<uint8_t*>(GameAddr(0x142AD09E7)), L ? *reinterpret_cast<uint8_t*>(L + 0x257c4) : 0xEE, L ? *reinterpret_cast<uint8_t*>(L + 0x257c0) : 0xEE, reinterpret_cast<void*>(hd), UiSceneActive(hd), L ? *reinterpret_cast<uint8_t*>(L + 0x2581f) : 0xEE, *reinterpret_cast<uint8_t*>(GameAddr(0x14227A81A))); }
            if (radar) {  // texture lookup for the player's tile, exactly like FUN_140690ef0
                const int ts = *reinterpret_cast<int*>(GameAddr(0x142BEE954)), ms = *reinterpret_cast<int*>(GameAddr(0x142BEE958));
                const float cx = *reinterpret_cast<float*>(GameAddr(0x142BCF6D0)), cz = *reinterpret_cast<float*>(GameAddr(0x142BCF6D4));
                const int tx = static_cast<int>(floorf((ms * 0.5f + cx) / ts));
                const int tz = static_cast<int>(ceilf((n - 1) - (ms * 0.5f + cz) / ts));
                const int idx = tz * n + tx;
                if (idx >= 0 && idx < 0x90) {
                    const uintptr_t s = radar + 0x138 + idx * 0x18;
                    char name[64];
                    sprintf_s(name, "streaming_worldMap_%d", idx + 1);
                    const uint32_t h0 = reinterpret_cast<uint32_t (*)(const char*, int)>(GameAddr(0x1400C7510))(name, 0);
                    const uint32_t h = ((h0 * 9) >> 0xb ^ h0 * 9) * 0x8001;
                    const uintptr_t txd = ReadPtr(s);
                    const uint32_t fl = *reinterpret_cast<uint32_t*>(s + 0xc);
                    const uintptr_t tex = (txd && (fl & 0x30000020) == 0x30000000) ? reinterpret_cast<uintptr_t (*)(uintptr_t, uint32_t)>(GameAddr(0x140151A00))(txd, h) : 0;
                    Log("  radar tile: (%d,%d) idx %d '%s' txd %p flags %08x -> texture %p", tx, tz, idx, name, reinterpret_cast<void*>(txd), fl, reinterpret_cast<void*>(tex));
                }
            }
            Log("  radar shader: fx=%p tech map=%d mask=%d drawMap=%d drawRadar=%d drawPath=%d vars diffuse=%d mask=%d aspect=%d maskAnim=%d", reinterpret_cast<void*>(ReadPtr(radar + 0xf78)), *reinterpret_cast<int*>(radar + 0xf80), *reinterpret_cast<int*>(radar + 0xf84), *reinterpret_cast<int*>(radar + 0xf88), *reinterpret_cast<int*>(radar + 0xf8c), *reinterpret_cast<int*>(radar + 0xf90), *reinterpret_cast<int*>(radar + 0xf94), *reinterpret_cast<int*>(radar + 0xf98), *reinterpret_cast<int*>(radar + 0xfb8), *reinterpret_cast<int*>(radar + 0xfbc));
            Log("  radar proj: 43b68=%.3f 44390=%.3f bee918..924=(%.3f %.3f %.3f %.3f) 43b00=(%.1f, %.1f) cc4c=%.1f", *reinterpret_cast<float*>(GameAddr(0x142C43B68)), *reinterpret_cast<float*>(GameAddr(0x142C44390)), *reinterpret_cast<float*>(GameAddr(0x142BEE918)), *reinterpret_cast<float*>(GameAddr(0x142BEE91C)), *reinterpret_cast<float*>(GameAddr(0x142BEE920)), *reinterpret_cast<float*>(GameAddr(0x142BEE924)), *reinterpret_cast<float*>(GameAddr(0x142C43B00)), *reinterpret_cast<float*>(GameAddr(0x142C43B04)), *reinterpret_cast<float*>(GameAddr(0x14227CC4C)));
            Log("  radar draw: mode(+0x130)=%d centre(6d0)=(%.1f, %.1f) 52a8=%d cc98=%.1f tileSize(954)=%d mapSize(958)=%d", radar ? *reinterpret_cast<int*>(radar + 0x130) : -9, *reinterpret_cast<float*>(GameAddr(0x142BCF6D0)), *reinterpret_cast<float*>(GameAddr(0x142BCF6D4)), *reinterpret_cast<uint8_t*>(GameAddr(0x142AC52A8)), *reinterpret_cast<float*>(GameAddr(0x14227CC98)), *reinterpret_cast<int*>(GameAddr(0x142BEE954)), *reinterpret_cast<int*>(GameAddr(0x142BEE958)));
            Log("  radar: d32e=%d d32f=%d cbff=%d grid %dx%d tiles: %d with id, %d loaded, %d other-state; player map (%.1f, %.1f)",
                *reinterpret_cast<uint8_t*>(GameAddr(0x14227D32E)), *reinterpret_cast<uint8_t*>(GameAddr(0x14227D32F)),
                *reinterpret_cast<uint8_t*>(GameAddr(0x14227CBFF)), n, n, valid, loaded, other,
                *reinterpret_cast<float*>(GameAddr(0x142BCF6C0)), *reinterpret_cast<float*>(GameAddr(0x142BCF6C4)));
        }
        if (const uintptr_t ui = ReadPtr(GameAddr(0x142C45610))) {  // UI renderer phase list
            const int count = *reinterpret_cast<int*>(ui + 0x180);
            for (int i = 0; i < 40; ++i) {
                const uintptr_t w = ReadPtr(ui + 0x30 + i * 8);
                if (!w) continue;
                Log("  ui[%d]%s vt=%p name=\"%.31s\" visible=%d movie=%p", i, i < count ? "" : "(over)",
                    reinterpret_cast<void*>(ReadPtr(w)), reinterpret_cast<const char*>(w + 0x68), *reinterpret_cast<uint8_t*>(w + 0x19),
                    reinterpret_cast<void*>(ReadPtr(w + 0x48)));
            }
        }
    }
}

void SkipMenus(const char* name) {
    static bool directDone = false;
    if (g_skipMenusMode == 2) {
        if (!directDone && strcmp(name, g_directStartEvent) == 0) {
            directDone = true;
            RunOnMainThread([] { StartGameDirect(); });
        }
        return;
    }
    static bool legalsTried = false, padSeen = false, titleSkipped = false;
    if (!legalsTried && strcmp(name, "fileDoFreeSpaceCheck") == 0) {
        legalsTried = true;  // legal screen is up; see if Enter skips it
        TapKeyLater(kScanEnter, 1500, "legal screen");
    } else if (strcmp(name, "startPadSelected") == 0) {
        padSeen = true;
    } else if (padSeen && !titleSkipped && strcmp(name, "OutTransitionComplete") == 0) {
        // Title screen ("Settings / Play / Quit") is up: press Play, then Normal.
        titleSkipped = true;
        HANDLE t = CreateThread(nullptr, 0, &PressPlayThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

void SandboxOnUiEvent(const char* name) {
    GameflowOnUiEvent(name);
    if (!g_enabled) return;
    if (g_skipMenus) SkipMenus(name);
    if (g_inSinglePlayer && strcmp(name, "InTransitionComplete") == 0) {  // debug: [debug] hide_scene=<UI scene name>
        LogActiveBootScenes(name);
        char sceneName[64] = {};
        GetPrivateProfileStringA("debug", "hide_scene", "", sceneName, sizeof(sceneName), "rdmp.ini");
        wchar_t w[64] = {};
        GetPrivateProfileStringW(L"debug", L"hide_scene", L"", w, 64, g_iniPath);
        for (int i = 0; i < 63 && w[i]; ++i) sceneName[i] = static_cast<char>(w[i]);
        if (sceneName[0]) {
            const uintptr_t s = FindUiScene(sceneName);
            Log("debug: hide scene \"%s\" = %p active=%d", sceneName, reinterpret_cast<void*>(s), UiSceneActive(s));
            if (s) reinterpret_cast<void (*)(uintptr_t)>(ReadPtr(ReadPtr(s) + 0x158))(s);
        }
    }
    if (strcmp(name, "enterSinglePlayer") == 0) {
        g_inSinglePlayer = true;
        g_enteredSinglePlayer = true;
    } else if (g_worldLoading && g_autoFinish && !g_playerSpawned && strncmp(name, "Loading", 7) == 0 &&
               GetTickCount64() - g_worldLoadStartMs >= g_finishDelayMs) {
        // The loading screen posts LoadingBulletInOut/LoadingRevolverSpin repeatedly while it waits; once the
        // world has had time to stream in, keep the finish request set until loadComplete.
        static bool logged = false;
        if (!logged) {
            logged = true;
            RequestFinishLoading("sandbox auto");
        } else {
            const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
            if (loader) *reinterpret_cast<volatile uint8_t*>(loader + kLoaderFlags) |= 0x10;
        }
    } else if (g_worldLoading && strcmp(name, "loadComplete") == 0) {
        g_worldReady = true;
        if (!g_playerSpawned && (g_spawnPlayer || g_limboSpawn)) {
            // MTA-style join: park the player underground with the screen black. The streamer and the
            // camera need an actor, so this is the anchor; the script's spawnPlayer moves it and fades in.
            if (g_limboSpawn) {
                g_spawnPos = g_limboPos;
                g_limbo    = true;
            }
            g_playerSpawned = true;
            SpawnPlayer();
            if (g_limbo) {
                DisablePlayerControl("limbo");
                EngineFadeToBlack(0.0f, "limbo join");
            }
        }
    }
}

// ---- script natives (server-requested client actions; see src/shared/fw_api.h) ----
// These are the MTA-style client natives: spawnPlayer / fadeCamera / setCameraTarget. They are
// called from the network thread, so every engine touch is marshalled to the main thread.
//
// Engine calls used (addresses verified in re/NOTES.md and the code above):
//   FUN_140ad4ca0  Actor teleport (handle, pos, heading, warp, 0, 0)
//   FUN_140376e50  camera SetTarget(cam, actorHandle, 0)
//   FUN_140376f40  camera Reset(cam, 0, 0);  cam+0x860 = actor+0x188
//   FUN_14063b0a0  loader fade in  (loader, seconds, 1.0f, 0)
//   FUN_14063b1b0  loader fade out (loader, seconds, 1.0f, 0); colour FUN_14063b3c0

bool IsWorldReady() {
    return g_worldReady;
}

// Player control is what scripts asked for (toggleAllControls / spawnPlayer) AND not locked by our UI (ApplyPlayerControl).
bool g_scriptControl = true;
bool g_uiInputLock   = false;

void ScriptSpawnPlayer(const Vec3& pos, float heading) {
    RunOnMainThread([pos, heading] {
        g_scriptControl = true;  // spawning gives control (MTA: spawnPlayer)
        // The player is created on the first spawnPlayer call: nothing in the mod spawns it at boot
        // (rdmp.ini [sandbox] spawn_player=0), so the script owns the spawn point and the respawn.
        if (!g_playerSpawned) {
            g_spawnPos     = pos;
            g_spawnHeading = heading;
            g_spawnPlayer  = true;
            g_localDead    = false;
            SpawnPlayer();
            g_playerSpawned = true;
            AttachCameraToPlayer("script spawnPlayer");
            CameraToPlayer("script spawnPlayer");
            Log("script: spawnPlayer created the player at (%.2f, %.2f, %.2f) heading %.1f", pos.x, pos.y, pos.z, heading);
            return;
        }
        if (g_localDead) {
            g_spawnPos     = pos;
            g_spawnHeading = heading;
            g_localDead    = false;
            ReleaseDeadPlayer();  // else the dead body stays in the world next to the new player
            RespawnPlayer();
            Log("script: spawnPlayer respawned the player at (%.2f, %.2f, %.2f) heading %.1f", pos.x, pos.y, pos.z, heading);
            return;
        }
        if (!TeleportLocalPlayer(pos, heading)) {
            Log("script: spawnPlayer at (%.2f, %.2f, %.2f) failed (no player)", pos.x, pos.y, pos.z);
            return;
        }
        if (g_limbo) {
            g_limbo = false;
            EnablePlayerControl("script spawnPlayer");
            Log("script: spawnPlayer left limbo");
        }
        AttachCameraToPlayer("script spawnPlayer");
        CameraToPlayer("script spawnPlayer");
        Log("script: spawnPlayer moved the player to (%.2f, %.2f, %.2f) heading %.1f", pos.x, pos.y, pos.z, heading);
    });
}

void ScriptFadeCamera(bool fadeIn, float milliseconds) {
    RunOnMainThread([fadeIn, milliseconds] {
        const uintptr_t loader = *reinterpret_cast<uintptr_t*>(GameAddr(kLoaderPtr));
        if (!loader) return;
        const float seconds = milliseconds / 1000.0f;
        if (fadeIn) {
            reinterpret_cast<void (*)(uintptr_t, float, float, uint8_t)>(GameAddr(0x14063B0A0))(loader, seconds, 1.0f, 0);
            Log("script: fadeCamera(in, %.2f s)", seconds);
        }
        else {
            EngineFadeToBlack(seconds, "script fadeCamera");
        }
    });
}

// Player control is what scripts asked for (toggleAllControls) AND not locked by our UI (the chat input is open: the
// game reads the keyboard itself, so typing would otherwise move / crouch the player).

void ApplyPlayerControl(const char* why) {
    if (g_scriptControl && !g_uiInputLock) {
        EnablePlayerControl(why);
    } else {
        DisablePlayerControl(why);
    }
}

void ScriptSetPlayerControl(bool enabled) {
    RunOnMainThread([enabled] {
        g_scriptControl = enabled;
        if (enabled) g_limbo = false;
        ApplyPlayerControl("script setControl");
        Log("script: setPlayerControl(%d)", enabled ? 1 : 0);
    });
}

void SetUiInputLock(bool locked) {
    if (g_uiInputLock == locked) return;
    g_uiInputLock = locked;
    ApplyPlayerControl(locked ? "ui lock" : "ui unlock");
}

// The session ended (disconnected / kicked): black screen and no control until a script spawns the player again on the
// next server, like a player who just joined.
void ScriptLeaveSession() {
    RunOnMainThread([] {
        EngineFadeToBlack(0.5f, "left the server");
        g_scriptControl = false;
        g_limbo         = true; // the next server's spawnPlayer leaves limbo and gives control back
        ApplyPlayerControl("left the server");
    });
}


void ScriptSetPlayerHealth(float health) {
    // The engine never sees 0: writing 0 into the health component makes the game tear the actor down on the
    // next update, which is what crashed the client DLL during the wasted flow. Deaths are owned by the script
    // layer (playerWasted + respawn); the component only needs to be restored/limited.
    const float clamped = health < 1.0f ? 1.0f : health;
    RunOnMainThread([clamped] {
        const float health = clamped;

        uintptr_t actor = 0;
        Vec3 p{};
        if (!PlayerActorAndPos(actor, p)) return;
        const uintptr_t hc = ReadPtr(actor + 0x60);
        if (hc && IsReadable(hc + 0x20, 4)) {
            const float before = *reinterpret_cast<float*>(hc + 0x20);
            *reinterpret_cast<float*>(hc + 0x20) = health;
            Log("script: setPlayerHealth(%.0f) (was %.0f)", health, before);
        }
    });
}

void ScriptSetCameraTarget() {
    RunOnMainThread([] {
        AttachCameraToPlayer("script setCameraTarget");
        CameraToPlayer("script setCameraTarget");
        Log("script: setCameraTarget(player)");
    });
}

}  // namespace rdmp
