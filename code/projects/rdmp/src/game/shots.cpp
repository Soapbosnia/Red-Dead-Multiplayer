// Bullet sync (MTA's bullet sync, applied to RDR): every remote shot becomes one bullet from that player's puppet.
//
// A gun shoots when its fire animation reaches the clip's fire event: the event dispatcher (FUN_1403019b0) calls the
// weapon object's delegate (+0x8f8) with the event, FUN_140302090(weapon object, event), which takes the round from
// the clip (+0x12c, the event's cost at +0x64), fills a bullet record (FUN_140320180) and launches it from the muzzle
// (FUN_140302a50). A puppet's animation does not reliably get there (re/NOTES.md "Damage sync"), so for a puppet the
// animation's own call is dropped and each shot the remote's game reports is fired through the same function here.
#include "shots.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "puppet_player.h"
#include "sandbox.h"

namespace rdmp {
namespace {

constexpr uintptr_t kShootEvent = 0x140302090;  // FUN_140302090(weapon object, fire event)
// mov [rsp+10h],rdx; push rbp; push rbx; push rsi; push rdi; push r12..r15  (17 bytes, position independent)
constexpr uint8_t kShootPrologue[17] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57,
                                        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

using ShootFn = void (*)(uintptr_t weapon, uintptr_t event);
ShootFn g_shootOrig = nullptr;
bool g_firing = false;  // our own call (FirePuppetShot) is in progress
bool g_probe = false;

uintptr_t Ptr(uintptr_t a) { return IsReadable(a, 8) ? *reinterpret_cast<uintptr_t*>(a) : 0; }

// actor handle (index | generation << 16) of the actor that owns the weapon object (+0x20)
uint32_t WeaponOwner(uintptr_t weapon) { return IsReadable(weapon + 0x20, 4) ? *reinterpret_cast<uint32_t*>(weapon + 0x20) : 0; }

std::unordered_map<uint32_t, uint32_t> g_puppetByActorHandle;  // actor handle -> goh, puppets with bullet sync

// FUN_140ae5a40(health component, damage, attacker, ...): applies a hit's damage to an actor that is not invulnerable
// (the caller, 0x140ae798a, skips it for health component +0x150 bit 0x100) and plays its hit reaction. A puppet takes
// our hits through it, so it reacts the way the game reacts to a hit, but it keeps its health: what really happens to
// the remote player is decided in their own game (and comes back through sync).
constexpr uintptr_t kApplyDamage = 0x140AE5A40;
// mov [rsp+20h],rbx; mov [rsp+18h],r8d; push rbp; push rsi; push rdi; push r12  (15 bytes, position independent)
constexpr uint8_t kApplyDamagePrologue[15] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
using ApplyDamageFn = void (*)(uintptr_t health, float damage, uint32_t attacker, uintptr_t a4, uint32_t a5, void* a6, uint8_t a7,
                               uint32_t a8, void* hit);
ApplyDamageFn g_applyDamageOrig = nullptr;

bool IsPuppetHealth(uintptr_t health) {
    for (const auto& [handle, goh] : g_puppetByActorHandle) {
        const uintptr_t actor = ActorFromGohHandle(goh);
        if (actor && Ptr(actor + 0x60) == health) return true;
    }
    return false;
}

void ApplyDamageHook(uintptr_t health, float damage, uint32_t attacker, uintptr_t a4, uint32_t a5, void* a6, uint8_t a7, uint32_t a8,
                     void* hit) {
    if (!IsPuppetHealth(health) || !IsReadable(health + 0x20, 4)) {
        g_applyDamageOrig(health, damage, attacker, a4, a5, a6, a7, a8, hit);
        return;
    }
    float& hp = *reinterpret_cast<float*>(health + 0x20);
    const float before = hp;
    if (damage > before - 1.0f) damage = before > 1.0f ? before - 1.0f : 0.0f;  // never a lethal hit here
    if (g_probe) Log("shots: our hit on a puppet: %.1f damage (health %.1f), reaction only", damage, before);
    g_applyDamageOrig(health, damage, attacker, a4, a5, a6, a7, a8, hit);
    hp = before;
}

bool g_dbgTarget = false;  // [debug] aim_hold_target: the local player's own shots go at this point
float g_dbgPoint[3] = {};

uintptr_t ActorOfHandle(uint32_t handle) {  // actor handle -> actor (*0x142C457F8 entries: object, generation)
    const uintptr_t table = Ptr(GameAddr(0x142C457F8));
    const uintptr_t e = table ? table + static_cast<uintptr_t>(handle & 0xffff) * 16 : 0;
    if (!e || !IsReadable(e, 16) || *reinterpret_cast<uint16_t*>(e + 8) != static_cast<uint16_t>(handle >> 16)) return 0;
    return Ptr(e);
}

void ShootHook(uintptr_t weapon, uintptr_t event) {
    const uint32_t owner = WeaponOwner(weapon);
    const uintptr_t info = Ptr(GameAddr(0x1422DAF30));
    if (g_dbgTarget && info && IsReadable(info + 0x5ec, 4) && *reinterpret_cast<uint32_t*>(info + 0x5ec) == owner) {
        // [debug] a test copy's player really shoots at the other player (its camera does not turn there): its reticle
        // point is the target for the shot
        const uintptr_t actor = ActorOfHandle(owner), comp = actor ? Ptr(actor + 0xb8) : 0;
        if (comp && IsReadable(comp + 0x3800, 0x30)) {
            float saved[12];
            float* r = reinterpret_cast<float*>(comp + 0x3800);
            std::memcpy(saved, r, sizeof(saved));
            for (int i = 0; i < 3; ++i) r[i * 4] = g_dbgPoint[0], r[i * 4 + 1] = g_dbgPoint[1], r[i * 4 + 2] = g_dbgPoint[2];
            g_shootOrig(weapon, event);
            std::memcpy(r, saved, sizeof(saved));
            return;
        }
    }
    const auto it = g_puppetByActorHandle.find(owner);
    const bool puppet = it != g_puppetByActorHandle.end();
    if (g_probe) {
        const float clip = IsReadable(weapon + 0x12c, 4) ? *reinterpret_cast<float*>(weapon + 0x12c) : -1.0f;
        Log("shots: shot event, weapon %p (owner 0x%08x%s), clip %.0f%s", reinterpret_cast<void*>(weapon), owner,
            puppet ? ", puppet" : "", clip, puppet && !g_firing ? " - the animation's, dropped" : "");
    }
    if (puppet && !g_firing) return;  // the puppet's own animation: its shots come from the network (FirePuppetShot)
    g_shootOrig(weapon, event);
}

}  // namespace

void InstallShotHooks(bool probe) {
    g_probe = probe;
    g_shootOrig = reinterpret_cast<ShootFn>(
        DetourPic(GameAddr(kShootEvent), kShootPrologue, sizeof(kShootPrologue), reinterpret_cast<void*>(&ShootHook)));
    Log("shots: shot hook %s", g_shootOrig ? "installed" : "FAILED (prologue differs)");
    g_applyDamageOrig = reinterpret_cast<ApplyDamageFn>(DetourPic(GameAddr(kApplyDamage), kApplyDamagePrologue, sizeof(kApplyDamagePrologue),
                                                                  reinterpret_cast<void*>(&ApplyDamageHook)));
    Log("shots: damage hook %s", g_applyDamageOrig ? "installed" : "FAILED (prologue differs)");
}

uintptr_t WeaponObject(uintptr_t actor) {
    // actor +0x70 -> +0x80 the weapon in hand (+0x460 when none), its vfunc +0xd8 -> +0xa0 the weapon object
    // (IS_ACTOR_SHOOTING reads its +0x120 bit 0 the same way)
    const uintptr_t inv = Ptr(actor + 0x70);
    uintptr_t w = inv ? Ptr(inv + 0x80) : 0;
    if (!w && inv) w = Ptr(inv + 0x460);
    if (!w || !Ptr(w)) return 0;
    using Fn = uintptr_t (*)(uintptr_t);
    const uintptr_t info = reinterpret_cast<Fn>(Ptr(Ptr(w) + 0xd8))(w);
    return info ? Ptr(info + 0xa0) : 0;
}

uintptr_t FireEvent(uintptr_t weapon) {
    // the weapon's clip definition: *(+0x28) +0x30[+0x30 index] (or the first); its fire events +0x98, count +0xa0
    const uintptr_t defs = Ptr(weapon + 0x28);
    if (!defs || !IsReadable(weapon + 0x30, 4)) return 0;
    const int idx = *reinterpret_cast<int*>(weapon + 0x30);
    uintptr_t def = (idx >= 0 && idx < 16) ? Ptr(defs + 0x30 + static_cast<uintptr_t>(idx) * 8) : 0;
    if (!def) def = Ptr(defs + 0x30);
    if (!def || !IsReadable(def + 0x98, 0x10) || *reinterpret_cast<int*>(def + 0xa0) <= 0) return 0;
    return Ptr(Ptr(def + 0x98));
}

void SetDebugShotTarget(bool on, float x, float y, float z) {
    g_dbgTarget = on;
    g_dbgPoint[0] = x, g_dbgPoint[1] = y, g_dbgPoint[2] = z;
}

void SetBulletSync(uint32_t goh, uint32_t actorHandle, bool on) {
    if (on) g_puppetByActorHandle[actorHandle] = goh;
    else g_puppetByActorHandle.erase(actorHandle);
}

bool FirePuppetShot(uint32_t goh) {
    if (!g_shootOrig) return false;
    const uintptr_t actor = ActorFromGohHandle(goh);
    const uintptr_t weapon = actor ? WeaponObject(actor) : 0;
    const uintptr_t event = weapon ? FireEvent(weapon) : 0;
    if (!event) {
        if (g_probe) Log("shots: puppet 0x%08x: no weapon object / fire event (%p)", goh, reinterpret_cast<void*>(weapon));
        return false;
    }
    if (g_probe) Log("shots: puppet 0x%08x fires a synced shot (clip %.0f)", goh, *reinterpret_cast<float*>(weapon + 0x12c));
    g_firing = true;
    g_shootOrig(weapon, event);
    g_firing = false;
    return true;
}

}  // namespace rdmp
