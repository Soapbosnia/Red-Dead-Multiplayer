// Weapons: see weapons.h.
//
// GIVE_WEAPON_TO_ACTOR(actor, weapon, 1, notify, hidden): the third argument must be 1 for the weapon to be usable from
// script (with 0 it is in the weapon wheel but ACTOR_PUT_WEAPON_IN_HAND refuses it). ACTOR_PUT_WEAPON_IN_HAND only
// files a request on the actor's inventory and fails until the item exists, so the local equip retries for a while.
// GET_WEAPON_IN_HAND is -1 once the game holsters the weapon (it does after a couple of seconds without aiming).
#include "weapons.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "bindings.h"
#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "puppet_player.h"
#include "sandbox.h"
#include "shots.h"
#include "scriptnatives.h"
#include "shared/weapons.h"
#include "watch.h"

namespace rdmp {
namespace {

bool g_probe = false;
int g_aimHold = 0;      // [debug] aim_hold: the game copy (InstanceId) that sends the aim input held (two-copy tests)
float g_aimHoldYaw = 0.0f, g_aimHoldSweep = 0.0f, g_aimHoldPitch = 0.0f;  // [debug] aim_hold_* (AimHoldPoint)
bool g_aimHoldPitchSwing = false;
bool g_aimHoldPoint = false;  // [debug] aim_hold_point=1: send a test point instead of the real crosshair
bool g_pitchProbe = false;
int g_autoWeapon = -1;  // [debug] auto_weapon
int g_aimHoldFireMs = 0;  // [debug] aim_hold_fire_ms
int g_aimHoldPeriodMs = 0;  // [debug] aim_hold_period_ms
bool g_syncProbe = false;  // [debug] sync_probe
int g_moveTest = 0;        // [debug] move_test
int g_inputProbe = 0;      // [debug] input_probe
int g_groundWatch = 0;  // [debug] ground_watch
int g_puppetFactionStatus = 0;  // [debug] puppet_faction_status: the player faction's status toward a puppet
bool g_aimHoldTarget = false;  // [debug] aim_hold_target=1: aim at the first remote player
int g_fireProbe = 0;  // [debug] fire_probe=<input id>: watch who reads that local input record
uintptr_t g_execWatch = 0;  // [debug] exec_watch=0x14...: execute breakpoint, logs its first calls (distinct rcx)
bool g_aimTrace = false;  // [debug] aim_trace: log a puppet's wanted / actual heading every 100 ms while it aims
uintptr_t g_execBps[4] = {};  // [debug] exec_bps
int g_execBpCount = 0;

uint32_t LocalActor() {
    NativeCall c;
    c.Int(0);
    return c.Call(NativeHash("GET_PLAYER_ACTOR")) ? c.ResultUint() : 0;
}

int InHand(uint32_t actor) {
    NativeCall w;
    w.Uint(actor);
    return w.Call(NativeHash("GET_WEAPON_IN_HAND")) ? w.ResultInt() : -1;
}

// The ammo type a weapon fires (its reserve lives in the inventory by ammo type, not on the weapon).
int AmmoTypeOf(int weapon) {
    NativeCall t;
    t.Int(weapon);
    return t.Call(NativeHash("GET_AMMOENUM_FOR_WEAPONENUM")) ? t.ResultInt() : -1;
}

void SetAmmo(uint32_t actor, int weapon, float ammo) {
    NativeCall a;
    a.Uint(actor).Int(weapon).Float(ammo);
    a.Call(NativeHash("ACTOR_SET_WEAPON_AMMO"));
    const int type = AmmoTypeOf(weapon);
    if (type >= 0) {
        NativeCall inv;
        inv.Uint(actor).Int(type).Float(ammo).Bool(false);
        inv.Call(NativeHash("ACTOR_SET_INV_AMMO"));
    }
}

// The inventory refuses to draw a weapon whose equip slot (0..6) is disabled (the bitmask at inventory +0x48e,
// FUN_140341210), and the story state leaves the long-gun slots off: enable them all with every give.
void EnableSlots(uint32_t actor) {
    for (int slot = 0; slot <= 6; ++slot) {
        NativeCall e;
        e.Uint(actor).Int(slot).Bool(true);
        const bool ok = e.Call(NativeHash("SET_EQUIP_SLOT_ENABLED"));
        if (g_probe) Log("weapon probe: equip slot %d enable %s", slot, ok ? "ok" : "failed");
    }
}

void Give(uint32_t actor, int weapon, float ammo) {
    EnableSlots(actor);
    NativeCall g;
    g.Uint(actor).Int(weapon).Int(1).Bool(false).Bool(false);
    g.Call(NativeHash("GIVE_WEAPON_TO_ACTOR"));
    SetAmmo(actor, weapon, ammo);
    if (g_probe) {
        const int type = AmmoTypeOf(weapon);
        for (const char* n : {"GET_AMMOENUM_FOR_WEAPONENUM", "ACTOR_SET_WEAPON_AMMO", "ACTOR_SET_INV_AMMO", "ACTOR_GET_INV_AMMO", "GET_LAST_ATTACKER"}) {
            Log("weapon probe: native %s %s", n, ResolveNative(NativeHash(n)) ? "resolves" : "MISSING");
        }
        NativeCall gi, gw;
        gi.Uint(actor).Int(type).Bool(false);
        gw.Uint(actor).Int(weapon);
        const float inv = gi.Call(NativeHash("ACTOR_GET_INV_AMMO")) ? gi.ResultFloat() : -9.0f;
        const float onWeapon = gw.Call(NativeHash("ACTOR_GET_WEAPON_AMMO")) ? gw.ResultFloat() : -9.0f;
        Log("weapon probe: gave %d: ammo type %d, inventory %.0f, weapon %.0f", weapon, type, inv, onWeapon);
    }
}

bool PutInHand(uint32_t actor, int weapon, bool inHand) {
    NativeCall n;  // the weapon the next draw takes (aiming draws it too)
    n.Uint(actor).Int(weapon);
    n.Call(NativeHash("ACTOR_SET_NEXT_EQUIP_SLOT_FROM_WEAPON_ENUM"));
    NativeCall h;
    h.Uint(actor).Int(weapon).Bool(inHand);
    return h.Call(NativeHash("ACTOR_PUT_WEAPON_IN_HAND")) && (h.ResultInt() & 0xff) != 0;
}

// A local equip still to happen: retried each frame until the weapon is in hand or the time is up.
int g_pendingWeapon = -1;
ULONGLONG g_pendingUntil = 0;

struct PuppetWeapon {
    uint32_t given = 0;     // bit per weapon id given to this actor
    int lastAsked = -2;     // the remote weapon last applied
    ULONGLONG nextTry = 0;  // throttles equip / holster attempts
    bool setUp = false;     // invulnerable (damage is decided in the victim's own game)
    bool haveShots = false;  // bullet sync: the remote's shot count already fired here
    uint8_t shots = 0;
};
std::unordered_map<uint32_t, PuppetWeapon> g_puppets;

// [debug] state_probe=1: the local player's weapon state, logged on every change (what full state sync must carry).
bool g_stateProbe = false;
int NativeInt(const char* name, uint32_t actor) {
    NativeCall n;
    n.Uint(actor);
    return n.Call(NativeHash(name)) ? n.ResultInt() : -99;
}
void StateProbeFrame(uint32_t actor) {
    const uintptr_t info = *reinterpret_cast<uintptr_t*>(GameAddr(0x1422DAF30));
    if (!info) return;
    const uintptr_t s410 = *reinterpret_cast<uintptr_t*>(info + 0x410);
    char line[256];
    sprintf_s(line, "zoom %d | 3bc %02x 3e4 %02x 3e0 %.2f | inhand %d drawn %d holstered %d shooting %d reloading %d blind %d",
              s410 ? *reinterpret_cast<int*>(s410 + 0x14) : -1, *reinterpret_cast<uint8_t*>(info + 0x3bc),
              *reinterpret_cast<uint8_t*>(info + 0x3e4), *reinterpret_cast<float*>(info + 0x3e0), InHand(actor),
              NativeInt("IS_WEAPON_DRAWN", actor) & 0xff, NativeInt("ACTOR_IS_HOLSTERED", actor) & 0xff,
              NativeInt("IS_ACTOR_SHOOTING", actor) & 0xff, NativeInt("IS_ACTOR_RELOADING", actor) & 0xff,
              NativeInt("IS_ACTOR_BLINDFIRING", actor) & 0xff);
    const int w = InHand(actor);
    if (weapons::Valid(w)) {
        NativeCall a, i;
        a.Uint(actor).Int(w);
        i.Uint(actor).Int(AmmoTypeOf(w)).Bool(false);
        const float clip = a.Call(NativeHash("ACTOR_GET_WEAPON_AMMO")) ? a.ResultFloat() : -1.0f;
        const float inv = i.Call(NativeHash("ACTOR_GET_INV_AMMO")) ? i.ResultFloat() : -1.0f;
        const size_t len = strlen(line);
        sprintf_s(line + len, sizeof(line) - len, " | weapon ammo %.0f inv %.0f", clip, inv);
    }
    static std::string last;
    // the timer at +0x3e0 changes every frame: compare without it
    std::string key = line;
    const size_t t = key.find("3e0 "), e = key.find(" |", t);
    if (t != std::string::npos && e != std::string::npos) key.erase(t, e - t);
    if (key != last) {
        last = key;
        Log("state probe: %s", line);
    }
}

void ProbeFrame(uint32_t actor) {
    static bool listed = false;
    if (!listed) {
        listed = true;
        SetGameBinding("@GENERIC.TARGET", 1, 2, 0x25);  // aim also on K (automation cannot hold a mouse button and a key)
        SetGameBinding("@GENERIC.FIRE", 1, 2, 0x26);    // fire also on L
        for (int i = 0; i < weapons::kCount + 8; ++i) {
            NativeCall n;
            n.Int(i);
            if (!n.Call(NativeHash("GET_WEAPON_ENUM_STRING_FROM_ENUM"))) break;
            const auto p = static_cast<uintptr_t>(n.ResultRaw());
            Log("weapon probe: %d %s", i, p && IsReadable(p, 4) ? reinterpret_cast<const char*>(p) : "?");
        }
    }
    static PlayerControls last{};
    PlayerControls now{};
    if (CaptureLocalControls(now)) {
        for (int id = 0; id < static_cast<int>(sizeof(now.value)); ++id) {
            if (now.value[id] != last.value[id]) Log("weapon probe: input 0x%02x = %u", id, now.value[id]);
        }
        last = now;
    }
    static int lastInHand = -2;
    const int inHand = InHand(actor);
    if (inHand != lastInHand) {
        Log("weapon probe: in hand %d", inHand);
        lastInHand = inHand;
    }
}

}  // namespace

void InitWeapons(const wchar_t* iniPath) {
    g_probe = GetPrivateProfileIntW(L"debug", L"weapon_probe", 0, iniPath) != 0;
    g_aimHold = GetPrivateProfileIntW(L"debug", L"aim_hold", 0, iniPath);
    g_aimTrace = GetPrivateProfileIntW(L"debug", L"aim_trace", 0, iniPath) != 0;
    g_stateProbe = GetPrivateProfileIntW(L"debug", L"state_probe", 0, iniPath) != 0;
    g_fireProbe = GetPrivateProfileIntW(L"debug", L"fire_probe", 0, iniPath);
    g_autoWeapon = static_cast<int>(GetPrivateProfileIntW(L"debug", L"auto_weapon", static_cast<UINT>(-1), iniPath));
    g_aimHoldFireMs = static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_fire_ms", 0, iniPath));
    g_aimHoldPeriodMs = static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_period_ms", 0, iniPath));
    g_syncProbe = GetPrivateProfileIntW(L"debug", L"sync_probe", 0, iniPath) != 0;
    g_moveTest = static_cast<int>(GetPrivateProfileIntW(L"debug", L"move_test", 0, iniPath));
    g_inputProbe = static_cast<int>(GetPrivateProfileIntW(L"debug", L"input_probe", 0, iniPath));
    g_groundWatch = static_cast<int>(GetPrivateProfileIntW(L"debug", L"ground_watch", 0, iniPath));  // 1 local, 2 a puppet
    SetProbePlayerBit(GetPrivateProfileIntW(L"net", L"puppet_full_update", 1, iniPath) != 0);
    SetCoverAttachMode(static_cast<int>(GetPrivateProfileIntW(L"debug", L"cover_attach_mode", 1, iniPath)));
    g_puppetFactionStatus = static_cast<int>(GetPrivateProfileIntW(L"debug", L"puppet_faction_status", 0, iniPath));
    g_aimHoldTarget = GetPrivateProfileIntW(L"debug", L"aim_hold_target", 0, iniPath) != 0;
    g_pitchProbe = GetPrivateProfileIntW(L"debug", L"pitch_probe", 0, iniPath) != 0;
    wchar_t hex[32] = {};
    GetPrivateProfileStringW(L"debug", L"exec_watch", L"", hex, 32, iniPath);
    g_execWatch = static_cast<uintptr_t>(wcstoull(hex, nullptr, 16));
    wchar_t bps[128] = {};
    GetPrivateProfileStringW(L"debug", L"exec_bps", L"", bps, 128, iniPath);
    for (wchar_t* q = bps; *q && g_execBpCount < 4;) {
        wchar_t* end = nullptr;
        const uintptr_t a = static_cast<uintptr_t>(wcstoull(q, &end, 16));
        if (end == q) break;
        if (a) g_execBps[g_execBpCount++] = a;
        q = *end ? end + 1 : end;
    }
    g_aimHoldYaw = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_yaw", 0, iniPath)));
    g_aimHoldSweep = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_sweep", 0, iniPath)));
    g_aimHoldPitch = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_pitch", 0, iniPath)));
    g_aimHoldPitchSwing = GetPrivateProfileIntW(L"debug", L"aim_hold_pitch_swing", 0, iniPath) != 0;
    g_aimHoldPoint = GetPrivateProfileIntW(L"debug", L"aim_hold_point", 0, iniPath) != 0;
}

// [debug] sync_probe=1: every 100 ms tick of the shared clock (two copies on one PC see the same GetTickCount64), the
// local player's state and every puppet's, the same fields, so tools/syncdiff.py can compare a player with their
// puppet in the other copy field by field.
void SyncProbeLine(const char* who, uint32_t actor, uint32_t goh, ULONGLONG tick) {
    Vec3 pos{};
    float heading = 0.0f;
    GetPuppetPose(goh, pos, heading);
    float hp = -1.0f;
    {
        NativeCall h;
        h.Uint(actor);
        if (h.Call(NativeHash("GET_ACTOR_HEALTH"))) hp = h.ResultFloat();
    }
    float aim[3] = {};
    if (const uintptr_t a = ActorFromGohHandle(goh)) {
        const uintptr_t comp = IsReadable(a + 0xb8, 8) ? *reinterpret_cast<uintptr_t*>(a + 0xb8) : 0;
        if (comp && IsReadable(comp + 0x3810, 12)) std::memcpy(aim, reinterpret_cast<const void*>(comp + 0x3810), 12);
    }
    static const char* kFlags[] = {"IS_ACTOR_ON_GROUND", "IS_ACTOR_JUMPING", "IS_ACTOR_CROUCHING", "IS_ACTOR_USING_COVER",
                                   "IS_ACTOR_RAGDOLL", "IS_ACTOR_REACTING", "IS_ACTOR_SHOOTING", "IS_ACTOR_RELOADING",
                                   "IS_WEAPON_DRAWN", "ACTOR_IS_HOLSTERED", "IS_ACTOR_USING_LEDGE", "IS_ACTOR_DEAD",
                                   "GET_ACTOR_GAIT_TYPE", "GET_ACTOR_POSTURE"};
    char flags[96];
    int k = 0;
    for (const char* f : kFlags) {
        NativeCall c;
        c.Uint(actor);
        const int v = c.Call(NativeHash(f)) ? (c.ResultInt() & 0xff) : -9;
        k += sprintf_s(flags + k, sizeof(flags) - k, "%d,", v);
    }
    // the character controller's ground probe (actor +0x88 -> +0x670: +0x10 contact point, +0x98 distance, +0xa6 on ground)
    float gy = 0.0f, gd = 0.0f;
    if (const uintptr_t a = ActorFromGohHandle(goh)) {
        const uintptr_t cc = IsReadable(a + 0x88, 8) ? *reinterpret_cast<uintptr_t*>(a + 0x88) : 0;
        if (cc && IsReadable(cc + 0x670, 0xa8)) gy = *reinterpret_cast<float*>(cc + 0x684), gd = *reinterpret_cast<float*>(cc + 0x708);
    }
    uintptr_t ccp = 0;
    unsigned b112 = 0, b118 = 0, f148 = 0;
    if (const uintptr_t a = ActorFromGohHandle(goh)) {
        ccp = IsReadable(a + 0x88, 8) ? *reinterpret_cast<uintptr_t*>(a + 0x88) : 0;
        b112 = *reinterpret_cast<uint8_t*>(a + 0x112), b118 = *reinterpret_cast<uint8_t*>(a + 0x118), f148 = *reinterpret_cast<uint32_t*>(a + 0x148);
    }
    Log("SP %s %llu pos %.3f %.3f %.3f h %.1f hp %.1f inhand %d aim %.2f %.2f %.2f f %s ground %.3f %.3f cc %p a112 %02x a118 %02x a148 %08x", who, tick,
        pos.x, pos.y, pos.z, heading, hp, InHand(actor), aim[0], aim[1], aim[2], flags, gy, gd, reinterpret_cast<void*>(ccp), b112, b118, f148);
}

void SyncProbeFrame() {
    static ULONGLONG last = 0;
    const ULONGLONG tick = GetTickCount64() / 100;
    if (tick == last) return;
    last = tick;
    const uint32_t local = LocalActor();
    if (local) SyncProbeLine("L", local, local, tick);
    for (const auto& [goh, pw] : g_puppets) SyncProbeLine("P", goh, goh, tick);
}

// [debug] move_test=<copy>: that copy's player runs a fixed input pattern (12 s loop, turning): walk 3 s, sprint 3 s (tapped),
// stop, a jump press (0x18 held), crouch (0x10 tap), walk while turning. Its real inputs, so the game moves it the way a
// player would and the other copy's puppet can be compared with it (sync_probe).
void MoveTestFrame() {
    static PlayerControls c;
    const double t = std::fmod(GetTickCount64() / 1000.0, 12.0);
    std::memset(c.value, 0, sizeof(c.value));
    c.value[0x11] = c.value[0x12] = 0x7f;  // the stick, centred
    if (t < 6.0 || (t >= 9.5 && t < 12.0)) c.value[0x12] = 0x00;           // forward
    if (t >= 3.0 && t < 6.0) c.value[0x18] = std::fmod(t, 0.4) < 0.2 ? 0xff : 0;  // sprint: tapped
    if (t >= 6.8 && t < 7.0) c.value[0x18] = 0xff;                          // a press standing: jump
    if ((t >= 8.0 && t < 8.15) || (t >= 9.2 && t < 9.35)) c.value[0x10] = 0xff;  // crouch on, off
    if (t >= 9.5) c.value[0x11] = 0xff;                                     // and turning right
    // The heading turns all the time (a full turn per 12 s loop), so the player walks circles of a few metres
    // instead of into the nearest wall.
    c.heading = static_cast<float>(t / 12.0 * 6.283185307179586 - 3.141592653589793);
    c.valid = true;
    SetLocalControlsOverride(&c);
}

// [debug] input_probe=<copy>: how cover is entered and left. An 8 s loop on that copy's player: tap the cover input
// (0x05) at 0 s, tap it again at 2 s, hold it at 4 s; the character controller's stance
// word (actor+0x88 -> +0xb40: 0xf00 nibble 1 = in cover, 0xf0 nibble 1 = crouched) is logged every 500 ms.
void InputProbeFrame() {
    static PlayerControls c;
    static int lastHalf = -1;
    const ULONGLONG ms = GetTickCount64();
    const int t = static_cast<int>(ms % 8000);
    const uintptr_t a = ActorFromGohHandle(LocalActor());
    const uintptr_t cc = a && IsReadable(a + 0x88, 8) ? *reinterpret_cast<uintptr_t*>(a + 0x88) : 0;
    const uint32_t word = cc && IsReadable(cc + 0xb40, 4) ? *reinterpret_cast<uint32_t*>(cc + 0xb40) : 0;
    const int half = static_cast<int>(ms / 500);
    if (half != lastHalf) {
        lastHalf = half;
        Log("input probe: t %.1f stance %08x", t / 1000.0, word);
    }
    std::memset(c.value, 0, sizeof(c.value));
    c.value[0x11] = c.value[0x12] = 0x7f;
    if (t < 200 || (t >= 2000 && t < 2200) || (t >= 4000 && t < 5000)) c.value[0x05] = 0xff;
    c.heading = 0.0f;
    c.valid = true;
    SetLocalControlsOverride(&c);
}

void WeaponsFrame() {
    if (g_inputProbe && g_inputProbe == InstanceId()) InputProbeFrame();
    if (g_syncProbe) SyncProbeFrame();
    static bool groundWatched = false;  // [debug] ground_watch=1: who writes the local player's on-ground flag
    if (g_groundWatch && !groundWatched && (g_groundWatch == 1 || !g_puppets.empty())) {
        const uintptr_t a = ActorFromGohHandle(g_groundWatch == 2 ? g_puppets.begin()->first : LocalActor()), cc = a && IsReadable(a + 0x88, 8) ? *reinterpret_cast<uintptr_t*>(a + 0x88) : 0;
        if (cc && IsReadable(cc + 0x714, 4)) {
            groundWatched = true;
            ArmWriteWatch(cc + 0x714, "on_ground");
        }
    }
    if (g_moveTest && g_moveTest == InstanceId()) MoveTestFrame();
    const uint32_t actor = LocalActor();
    if (!actor) return;
    if (g_pendingWeapon >= 0) {
        if (InHand(actor) == g_pendingWeapon || GetTickCount64() > g_pendingUntil) {
            g_pendingWeapon = -1;
        } else {
            PutInHand(actor, g_pendingWeapon, true);
        }
    }
    if (g_probe) ProbeFrame(actor);
    // [debug] auto_weapon=<id>: give the local player that weapon in hand 3 s after each spawn (tests without the UI)
    static ULONGLONG autoAt = 0;
    static bool autoDone = false;
    static uint32_t autoActor = 0;
    if (actor != autoActor) autoActor = actor, autoDone = false, autoAt = 0;  // again after every (re)spawn
    if (g_autoWeapon >= 0 && !autoDone) {
        if (!autoAt) autoAt = GetTickCount64() + 3000;
        else if (GetTickCount64() > autoAt) {
            autoDone = true;
            GiveLocalWeapon(g_autoWeapon, 50, true);
        }
    }
    if (g_stateProbe) {
        StateProbeFrame(actor);
        for (auto& [goh, pw] : g_puppets) {  // the puppets' state, same format: the two copies' logs line up
            char line[200];
            const int w = InHand(goh);
            float clip = -1.0f;
            if (weapons::Valid(w)) {
                NativeCall a;
                a.Uint(goh).Int(w);
                if (a.Call(NativeHash("ACTOR_GET_WEAPON_AMMO"))) clip = a.ResultFloat();
            }
            sprintf_s(line, "inhand %d drawn %d holstered %d shooting %d reloading %d | weapon ammo %.0f", w,
                      NativeInt("IS_WEAPON_DRAWN", goh) & 0xff, NativeInt("ACTOR_IS_HOLSTERED", goh) & 0xff,
                      NativeInt("IS_ACTOR_SHOOTING", goh) & 0xff, NativeInt("IS_ACTOR_RELOADING", goh) & 0xff, clip);
            static std::unordered_map<uint32_t, std::string> last;
            if (last[goh] != line) {
                last[goh] = line;
                Log("state probe: puppet 0x%08x %s", goh, line);
            }
        }
    }
    static bool fireArmed = false;  // [debug] fire_probe=1: log the code reading the local fire input record (0x09)
    if (g_fireProbe && !fireArmed) {
        const uintptr_t info = *reinterpret_cast<uintptr_t*>(GameAddr(0x1422DAF30));
        const uintptr_t ctrl = info ? *reinterpret_cast<uintptr_t*>(info + 0x118) : 0;
        const uintptr_t set = ctrl ? *reinterpret_cast<uintptr_t*>(ctrl + 0x20) : 0;
        const uintptr_t recs = set ? *reinterpret_cast<uintptr_t*>(set) : 0;
        if (recs) {
            fireArmed = true;
            ArmAccessWatch(recs + g_fireProbe * 16, "inputRecord");
        }
    }
    static bool pitchArmed = false;  // [debug] pitch_probe=1: who writes the aim blend weight *(*(motion+0x3b0)+0x150)+0x280
    if (g_pitchProbe && !pitchArmed) {
        const uintptr_t a = ActorFromGohHandle(actor);
        const uintptr_t mot = a ? *reinterpret_cast<uintptr_t*>(a + 0x38) : 0;
        const uintptr_t x = mot && IsReadable(mot + 0x3b0, 8) ? *reinterpret_cast<uintptr_t*>(mot + 0x3b0) : 0;
        const uintptr_t y = x && IsReadable(x + 0x150, 8) ? *reinterpret_cast<uintptr_t*>(x + 0x150) : 0;
        if (y && IsReadable(y + 0x280, 4)) {
            pitchArmed = true;
            Log("weapon probe: aim blend object %p", reinterpret_cast<void*>(y));
            ArmAccessWatch(y + 0x280, "aimBlendWeight");
        }
    }
    static bool execArmed = false;
    if (g_execWatch && !execArmed && !g_puppets.empty()) {  // once a puppet is around
        execArmed = true;
        ArmExecWatch(GameAddr(g_execWatch), "exec_watch");
    }
    static bool bpsArmed = false;  // [debug] exec_bps=0x..,0x..: up to 4 execute breakpoints (first calls logged)
    if (!bpsArmed && g_execBpCount && !g_puppets.empty()) {
        bpsArmed = true;
        uintptr_t addrs[4];
        for (int i = 0; i < g_execBpCount; ++i) addrs[i] = GameAddr(g_execBps[i]);
        ArmExecBreakpoints(addrs, g_execBpCount, "exec_bps");
    }
    SetLocalAimHeld(AimHoldDebug());  // [debug] aim_hold: this copy's own player really aims too
    SetLocalFirePulse(AimHoldDebug() ? static_cast<uint32_t>(g_aimHoldFireMs) : 0u);
}

void LocalWeapon(int8_t& weapon, uint16_t& ammo) {
    weapon = -1;
    ammo = 0;
    const uint32_t actor = LocalActor();
    if (!actor) return;
    const int w = InHand(actor);
    if (!weapons::Valid(w)) return;
    weapon = static_cast<int8_t>(w);
    const int type = AmmoTypeOf(w);  // the reserve is per ammo type; weapons without one (lasso, knife) count on themselves
    NativeCall a;
    if (type >= 0) a.Uint(actor).Int(type).Bool(false);
    else a.Uint(actor).Int(w);
    if (a.Call(NativeHash(type >= 0 ? "ACTOR_GET_INV_AMMO" : "ACTOR_GET_WEAPON_AMMO"))) {
        const float f = a.ResultFloat();
        ammo = static_cast<uint16_t>(f < 0 ? 0 : f > 65535.0f ? 65535.0f : f);
    }
}

void GiveLocalWeapon(int weapon, int ammo, bool inHand) {
    const uint32_t actor = LocalActor();
    if (!actor || !weapons::Valid(weapon)) return;
    Give(actor, weapon, static_cast<float>(ammo < 0 ? 0 : ammo));
    Log("weapons: given %s (%d ammo)%s", weapons::Name(weapon), ammo, inHand ? ", in hand" : "");
    if (inHand) {
        g_pendingWeapon = weapon;
        g_pendingUntil = GetTickCount64() + 2000;
    }
}

void TakeLocalWeapon(int weapon) {
    const uint32_t actor = LocalActor();
    if (!actor) return;
    NativeCall c;
    if (weapon < 0) {
        c.Uint(actor);
        c.Call(NativeHash("DELETE_ALL_WEAPONS_FROM_ACTOR"));
    } else if (weapons::Valid(weapon)) {
        c.Uint(actor).Int(weapon);
        c.Call(NativeHash("DELETE_WEAPON_FROM_ACTOR"));
    }
    if (g_pendingWeapon == weapon || weapon < 0) g_pendingWeapon = -1;
}

void SetLocalWeaponAmmo(int weapon, int ammo) {
    const uint32_t actor = LocalActor();
    if (!actor || !weapons::Valid(weapon)) return;
    SetAmmo(actor, weapon, static_cast<float>(ammo < 0 ? 0 : ammo));
}

void PutLocalWeaponInHand(int weapon) {
    const uint32_t actor = LocalActor();
    if (!actor) return;
    if (weapon < 0) {
        const int current = InHand(actor);
        if (weapons::Valid(current)) PutInHand(actor, current, false);
        g_pendingWeapon = -1;
        return;
    }
    if (!weapons::Valid(weapon)) return;
    g_pendingWeapon = weapon;
    g_pendingUntil = GetTickCount64() + 2000;
}

void SyncPuppetWeapon(uint32_t goh, int weapon) {
    if (!goh) return;
    SetPlayerArmed(goh, weapons::Valid(weapon));
    SetPlayerGun(goh, weapons::IsGun(weapon));
    PuppetWeapon& p = g_puppets[goh];
    if (!p.setUp) {
        p.setUp = true;
        // Not invulnerable, so our hits play the game's hit reaction on it; the damage hook (shots.cpp) keeps its health:
        // the remote player's own game decides their damage (as in MTA).
        NativeCall inv;
        inv.Uint(goh).Bool(false);
        inv.Call(NativeHash("SET_ACTOR_INVULNERABILITY"));
        // The player's faction (2) treats this actor as hostile: a remote player is the puppet's faction too, and the
        // game drops a player's hits on its own faction (no damage call, no reaction).
        NativeCall fs;
        fs.Int(2).Uint(goh).Int(g_puppetFactionStatus);
        fs.Call(NativeHash("SET_FACTION_STATUS_TO_INDIVIDUAL_ACTOR"));
        SetBulletSync(goh, ActorHandleFromGohHandle(goh), true);
        // The update priority the local player has (an NPC's lower one let the game switch the puppet between update
        // levels, resetting its character controller each time: re/NOTES.md "Measuring the sync")
        NativeCall lp, pp;
        lp.Uint(LocalActor());
        pp.Uint(goh);
        const int localPrio = lp.Call(NativeHash("GET_ACTOR_UPDATE_PRIORITY")) ? lp.ResultInt() : -99;
        const int puppetPrio = pp.Call(NativeHash("GET_ACTOR_UPDATE_PRIORITY")) ? pp.ResultInt() : -99;
        if (localPrio != -99) {
            NativeCall sp;
            sp.Uint(goh).Int(localPrio);
            sp.Call(NativeHash("SET_ACTOR_UPDATE_PRIORITY"));
        }
        Log("weapons: puppet 0x%08x update priority %d -> %d (the local player's)", goh, puppetPrio, localPrio);
    }
    const ULONGLONG now = GetTickCount64();
    if (weapon != p.lastAsked) {
        p.lastAsked = weapon;
        p.nextTry = 0;
    }
    if (now < p.nextTry) return;
    p.nextTry = now + 250;
    const int inHand = InHand(goh);
    if (weapons::Valid(weapon)) {
        if (!(p.given & (1u << weapon))) {
            Give(goh, weapon, 999.0f);
            p.given |= 1u << weapon;
            return;  // the item exists from the next frame
        }
        // unlimited reserve for the puppet (the remote's own game counts their ammo); the clip follows the remote's
        // (SyncPuppetWeaponState)
        const int type = AmmoTypeOf(weapon);
        NativeCall a;
        a.Uint(goh).Int(type).Bool(false);
        if (type >= 0 && a.Call(NativeHash("ACTOR_GET_INV_AMMO")) && a.ResultFloat() < 200.0f) {
            NativeCall inv;
            inv.Uint(goh).Int(type).Float(999.0f).Bool(false);
            inv.Call(NativeHash("ACTOR_SET_INV_AMMO"));
        }
        if (inHand != weapon) {
            const bool ok = PutInHand(goh, weapon, true);
            if (g_aimTrace) Log("aim trace: puppet 0x%08x in hand %d, put %d in hand -> %d", goh, inHand, weapon, ok ? 1 : 0);
        }
    } else if (weapons::Valid(inHand)) {
        PutInHand(goh, inHand, false);
        if (g_aimTrace) Log("aim trace: puppet 0x%08x holstered %d", goh, inHand);
    }
}

void ForgetPuppetWeapon(uint32_t goh) {
    SetBulletSync(goh, ActorHandleFromGohHandle(goh), false);
    g_puppets.erase(goh);
}

// The aim point is the reticle target point GET_RETICLE_TARGET_POINT reads: the actor's +0xb8 component (one of the two
// player components; puppets get theirs in AttachPlayerControl) at +0x3810 (x, y, z, w), copies at +0x3800 / +0x3820.
// The local player's update writes it every frame (FUN_140393f22, from FUN_140348600) and the weapon code reads it.
// A puppet gets the remote's (puppet_player.cpp SetPlayerAim), plus aim mode and a heading towards it (below).
namespace {
uintptr_t AimComponent(uint32_t goh) {
    const uintptr_t actor = goh ? ActorFromGohHandle(goh) : 0;
    const uintptr_t comp = actor && IsReadable(actor + 0xb8, 8) ? *reinterpret_cast<uintptr_t*>(actor + 0xb8) : 0;
    return comp && IsReadable(comp + 0x3810, 0x20) ? comp : 0;
}
}  // namespace

void AimHoldPoint(const Vec3& pos, double nowMs, float& x, float& y, float& z) {
    if (!g_aimHoldPoint) return;  // the real crosshair point
    const double s = nowMs / 1000.0;
    const float yaw = (g_aimHoldYaw + g_aimHoldSweep * static_cast<float>(std::fmod(s, 360.0))) * 0.01745329f;
    const float pitch = (g_aimHoldPitchSwing ? g_aimHoldPitch * static_cast<float>(std::sin(s * 1.5707963)) : g_aimHoldPitch) * 0.01745329f;
    x = pos.x - std::sin(yaw) * 20.0f;
    z = pos.z - std::cos(yaw) * 20.0f;
    y = pos.y + 1.5f + std::tan(pitch) * 20.0f;
}
bool AimHoldAtRemote() { return g_aimHoldTarget; }
bool SyncProbeOn() { return g_syncProbe; }

void FaceLocalPlayerAt(const Vec3& from, const Vec3& to) {  // [debug] aim_hold_target: the player faces the target
    const float dx = to.x - from.x, dz = to.z - from.z;
    if (dx * dx + dz * dz < 0.04f) return;
    // only without a gun (melee): while aiming the camera turns the player, and turning it as well makes it twitch
    if (weapons::IsGun(InHand(LocalActor()))) return;
    const float want = std::atan2(-dx, -dz) * 57.2957795f;  // the engine's heading faces (-sin h, -cos h)
    float err = want - ActorHeadingDeg(ActorFromGohHandle(LocalActor()));
    while (err > 180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    if (std::fabs(err) < 20.0f) return;  // close enough: leave the animations alone
    NativeCall h;
    h.Uint(LocalActor()).Float(want).Bool(false);
    h.Call(NativeHash("SET_ACTOR_HEADING"));
}
bool AimHoldDebug() {  // -1 = every copy; aim_hold_period_ms: held the first half of each period only
    if (g_aimHold == 0 || (g_aimHold != InstanceId() && g_aimHold > 0)) return false;
    return g_aimHoldPeriodMs <= 0 || GetTickCount64() % static_cast<ULONGLONG>(g_aimHoldPeriodMs) < static_cast<ULONGLONG>(g_aimHoldPeriodMs / 2);
}

namespace {
float g_expectHealth = -1.0f;  // a health a script set (ExpectLocalHealth): reaching it is no hit
ULONGLONG g_expectUntil = 0;   // ... within a second
}  // namespace

void ExpectLocalHealth(float health) {
    g_expectHealth = health;
    g_expectUntil = GetTickCount64() + 1000;
}

bool PollLocalHit(float health, LocalHit& out) {
    static float last = -1.0f;
    static uint32_t lastActor = 0;
    if (g_expectHealth >= 0.0f && GetTickCount64() > g_expectUntil) g_expectHealth = -1.0f;
    if (g_expectHealth >= 0.0f && std::fabs(health - g_expectHealth) < 0.01f) {
        g_expectHealth = -1.0f;
        last = health;
        return false;
    }
    const uint32_t actor = LocalActor();
    if (!actor) {
        last = -1.0f;
        return false;
    }
    if (actor != lastActor || last < 0.0f || health > last) {  // new actor (spawn), first frame, healing / setHealth
        lastActor = actor;
        last = health;
        return false;
    }
    if (health >= last) return false;
    out = LocalHit{};
    out.before = last;
    out.health = health;
    out.loss = last - health;
    last = health;
    NativeCall a, w, z;
    a.Uint(actor);
    if (a.Call(NativeHash("GET_LAST_ATTACKER"))) out.attacker = a.ResultUint();
    w.Uint(actor);
    if (w.Call(NativeHash("GET_LAST_HIT_WEAPON"))) out.weapon = w.ResultInt();
    int zone = -1;
    z.Uint(actor).Ptr(&zone);
    if (z.Call(NativeHash("GET_LAST_HIT_ZONE")) && z.ResultBool()) out.bodyPart = zone;
    if (!weapons::Valid(out.weapon)) out.weapon = -1;
    NativeCall clear;
    clear.Uint(actor);
    clear.Call(NativeHash("CLEAR_LAST_HIT"));
    return true;
}

void LocalWeaponState(WeaponState& out) {
    out = WeaponState{};
    const uint32_t actor = LocalActor();
    const uintptr_t info = *reinterpret_cast<uintptr_t*>(GameAddr(0x1422DAF30));
    if (!actor || !info) return;
    const uintptr_t s410 = *reinterpret_cast<uintptr_t*>(info + 0x410);
    const int aim = s410 && IsReadable(s410 + 0x14, 4) ? *reinterpret_cast<int*>(s410 + 0x14) : 0;
    out.aimState = static_cast<uint8_t>(aim < 0 ? 0 : aim > 7 ? 7 : aim);
    const float ready = *reinterpret_cast<float*>(info + 0x3e0) * 20.0f;
    out.readyTicks = static_cast<uint8_t>(ready <= 0.0f ? 0.0f : ready >= 255.0f ? 255.0f : ready + 0.5f);
    const int w = InHand(actor);
    const bool reloading = (NativeInt("IS_ACTOR_RELOADING", actor) & 0xff) != 0;
    if ((NativeInt("IS_WEAPON_DRAWN", actor) & 0xff) != 0) out.flags |= 1;
    if (reloading) out.flags |= 2;
    if (ActorInCover(ActorFromGohHandle(actor))) out.flags |= WeaponState::kInCover;
    static int lastWeapon = -1, lastClip = -1;
    static uint8_t shots = 0;
    int clip = -1;
    if (weapons::Valid(w)) {
        NativeCall a;
        a.Uint(actor).Int(w);
        if (a.Call(NativeHash("ACTOR_GET_WEAPON_AMMO"))) clip = static_cast<int>(a.ResultFloat() + 0.5f);
    }
    if (w == lastWeapon && clip >= 0 && lastClip >= 0 && clip < lastClip && !reloading) shots += static_cast<uint8_t>(lastClip - clip);
    lastWeapon = w;
    lastClip = clip;
    out.shots = shots;
    out.clip = static_cast<uint8_t>(clip < 0 ? 0 : clip > 255 ? 255 : clip);
}

void LocalCamera(float& x, float& y, float& z) {
    const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(GameAddr(0x142C45738));
    const uintptr_t frame = mgr && IsReadable(mgr + 8, 8) ? *reinterpret_cast<uintptr_t*>(mgr + 8) : 0;
    if (!frame || !IsReadable(frame + 0x200, 12)) return;
    const float* p = reinterpret_cast<const float*>(frame + 0x200);
    x = p[0], y = p[1], z = p[2];
}

void SyncPuppetWeaponState(uint32_t goh, int weapon, const WeaponState& state, const Vec3& shotFrom, const Vec3& shotTo) {
    SetPlayerWeaponState(goh, state);
    if (!HasPlayerControl(goh) || !weapons::IsGun(weapon) || InHand(goh) != weapon) return;
    // (The bullets themselves come from the shot events, netclient.cpp FireRemoteShots; the shot counter only plays the
    // puppet's fire animation, puppet_player.cpp.)
    (void)shotFrom, (void)shotTo;
    const int want = state.clip + PendingPuppetShots(goh);
    NativeCall a;
    a.Uint(goh).Int(weapon);
    if (a.Call(NativeHash("ACTOR_GET_WEAPON_AMMO")) && static_cast<int>(a.ResultFloat() + 0.5f) != want && !(state.flags & 2)) {
        NativeCall s;
        s.Uint(goh).Int(weapon).Float(static_cast<float>(want));
        s.Call(NativeHash("ACTOR_SET_WEAPON_AMMO"));
    }
}

void LocalAim(float& x, float& y, float& z) {
    if (const uintptr_t comp = AimComponent(LocalActor())) {
        const float* p = reinterpret_cast<const float*>(comp + 0x3810);
        x = p[0], y = p[1], z = p[2];
    }
}

void SyncPuppetAim(uint32_t goh, const Vec3& aim, const Vec3& from, float bodyHeading, bool aiming) {
    SetPlayerAim(goh, aim.x, aim.y, aim.z, from.x, from.y, from.z);
    if (!aiming || !HasPlayerControl(goh)) return;
    if (g_aimTrace) {
        const ULONGLONG now = GetTickCount64();
        static ULONGLONG next = 0;
        if (now >= next) {
            next = now + 100;
            Log("aim trace: puppet 0x%08x remote heading %.1f, puppet %.1f, aim (%.2f, %.2f, %.2f)", goh, bodyHeading,
                ActorHeadingDeg(ActorFromGohHandle(goh)), aim.x, aim.y, aim.z);
        }
    }
    NativeCall h;
    h.Uint(goh).Float(bodyHeading).Bool(false);
    h.Call(NativeHash("SET_ACTOR_HEADING"));
}

}  // namespace rdmp
