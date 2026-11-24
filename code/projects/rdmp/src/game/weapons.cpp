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
int g_fireProbe = 0;  // [debug] fire_probe=<input id>: watch who reads that local input record
uintptr_t g_execWatch = 0;  // [debug] exec_watch=0x14...: execute breakpoint, logs its first calls (distinct rcx)
bool g_aimTrace = false;  // [debug] aim_trace: log a puppet's wanted / actual heading every 100 ms while it aims

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
    wchar_t hex[32] = {};
    GetPrivateProfileStringW(L"debug", L"exec_watch", L"", hex, 32, iniPath);
    g_execWatch = static_cast<uintptr_t>(wcstoull(hex, nullptr, 16));
    g_aimHoldYaw = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_yaw", 0, iniPath)));
    g_aimHoldSweep = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_sweep", 0, iniPath)));
    g_aimHoldPitch = static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"debug", L"aim_hold_pitch", 0, iniPath)));
    g_aimHoldPitchSwing = GetPrivateProfileIntW(L"debug", L"aim_hold_pitch_swing", 0, iniPath) != 0;
    g_aimHoldPoint = GetPrivateProfileIntW(L"debug", L"aim_hold_point", 0, iniPath) != 0;
}

void WeaponsFrame() {
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
    static bool execArmed = false;
    if (g_execWatch && !execArmed && !g_puppets.empty()) {  // once a puppet is around
        execArmed = true;
        ArmExecWatch(GameAddr(g_execWatch), "exec_watch");
    }
    SetLocalAimHeld(AimHoldDebug());  // [debug] aim_hold: this copy's own player really aims too
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
    PuppetWeapon& p = g_puppets[goh];
    if (!p.setUp) {
        p.setUp = true;
        NativeCall inv;  // our shots must not kill a remote player here: their own game decides their damage
        inv.Uint(goh).Bool(true);
        inv.Call(NativeHash("SET_ACTOR_INVULNERABILITY"));
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
bool AimHoldDebug() { return g_aimHold != 0 && g_aimHold == InstanceId(); }

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

void SyncPuppetWeaponState(uint32_t goh, int weapon, const WeaponState& state) {
    SetPlayerWeaponState(goh, state);
    if (!HasPlayerControl(goh) || !weapons::Valid(weapon) || InHand(goh) != weapon) return;
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
