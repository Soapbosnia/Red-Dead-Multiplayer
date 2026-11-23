// Weapons: see weapons.h.
//
// GIVE_WEAPON_TO_ACTOR(actor, weapon, 1, notify, hidden): the third argument must be 1 for the weapon to be usable from
// script (with 0 it is in the weapon wheel but ACTOR_PUT_WEAPON_IN_HAND refuses it). ACTOR_PUT_WEAPON_IN_HAND only
// files a request on the actor's inventory and fails until the item exists, so the local equip retries for a while.
// GET_WEAPON_IN_HAND is -1 once the game holsters the weapon (it does after a couple of seconds without aiming).
#include "weapons.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "bindings.h"
#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "puppet_player.h"
#include "scriptnatives.h"
#include "shared/weapons.h"

namespace rdmp {
namespace {

bool g_probe = false;

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

void Give(uint32_t actor, int weapon, float ammo) {
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

void ProbeFrame(uint32_t actor) {
    static bool listed = false;
    if (!listed) {
        listed = true;
        SetGameBinding("@GENERIC.TARGET", 1, 2, 0x25);  // aim also on K (automation cannot hold a mouse button and a key)
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
        NativeCall a;  // unlimited for the puppet: the remote's own game counts their ammo
        a.Uint(goh).Int(weapon);
        if (a.Call(NativeHash("ACTOR_GET_WEAPON_AMMO")) && a.ResultFloat() < 200.0f) SetAmmo(goh, weapon, 999.0f);
        if (inHand != weapon) PutInHand(goh, weapon, true);
    } else if (weapons::Valid(inHand)) {
        PutInHand(goh, inHand, false);
    }
}

void ForgetPuppetWeapon(uint32_t goh) {
    g_puppets.erase(goh);
}

}  // namespace rdmp
