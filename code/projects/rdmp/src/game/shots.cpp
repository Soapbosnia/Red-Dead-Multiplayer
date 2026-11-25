// Bullet sync (MTA's bullet sync, applied to RDR): every remote shot becomes one bullet from that player's puppet.
//
// A gun shoots when its fire animation reaches the clip's fire event: the event dispatcher (FUN_1403019b0) calls the
// weapon object's delegate (+0x8f8) with the event, FUN_140302090(weapon object, event), which takes the round from
// the clip (+0x12c, the event's cost at +0x64), fills a bullet record (FUN_140320180) and launches it from the muzzle
// (FUN_140302a50). A puppet's animation does not reliably get there (re/NOTES.md "Damage sync"), so for a puppet the
// animation's own call is dropped and each shot the remote's game reports is fired through the same function here.
#include "shots.h"

#include <windows.h>

#include <cmath>
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

uintptr_t ActorOfHandle(uint32_t handle) {  // actor handle -> actor (*0x142C457F8 entries: object, generation)
    const uintptr_t table = Ptr(GameAddr(0x142C457F8));
    const uintptr_t e = table ? table + static_cast<uintptr_t>(handle & 0xffff) * 16 : 0;
    if (!e || !IsReadable(e, 16) || *reinterpret_cast<uint16_t*>(e + 8) != static_cast<uint16_t>(handle >> 16)) return 0;
    return Ptr(e);
}

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

uint32_t PuppetOfHealth(uintptr_t health) {  // the goh whose actor has this health component, 0 = not a puppet
    for (const auto& [handle, goh] : g_puppetByActorHandle) {
        const uintptr_t actor = ActorFromGohHandle(goh);
        if (actor && Ptr(actor + 0x60) == health) return goh;
    }
    return 0;
}

LocalDamageFn g_localDamage = nullptr;

void ApplyDamageHook(uintptr_t health, float damage, uint32_t attacker, uintptr_t a4, uint32_t a5, void* a6, uint8_t a7, uint32_t a8,
                     void* hit) {
    // The local player: the hit is decided here, before the game applies it (MTA's onClientPlayerDamage), so a script
    // can cancel any hit, a lethal one too. a8 = the weapon (what GET_LAST_HIT_WEAPON reads back, health +0x140); the
    // hit zone is already in health +0x120 (valid when +0x138 is set, GET_LAST_HIT_ZONE).
    const uintptr_t info = Ptr(GameAddr(0x1422DAF30));
    const uint32_t localHandle = info && IsReadable(info + 0x5ec, 4) ? *reinterpret_cast<uint32_t*>(info + 0x5ec) : 0;
    const uintptr_t localActor = localHandle ? ActorOfHandle(localHandle) : 0;
    if (g_localDamage && localActor && Ptr(localActor + 0x60) == health && IsReadable(health + 0x20, 0x120)) {
        float& hp = *reinterpret_cast<float*>(health + 0x20);
        const float before = hp;
        const int zone = *reinterpret_cast<uint8_t*>(health + 0x138) ? *reinterpret_cast<int*>(health + 0x120) : -1;
        if (!g_localDamage(attacker, static_cast<int>(a8), zone, damage, before)) {
            if (g_probe) Log("shots: local hit (%.1f, weapon %d, zone %d, attacker 0x%08x) cancelled", damage, static_cast<int>(a8), zone, attacker);
            return;
        }
        g_applyDamageOrig(health, damage, attacker, a4, a5, a6, a7, a8, hit);
        return;
    }
    const uint32_t puppet = PuppetOfHealth(health);
    if (!puppet || !IsReadable(health + 0x20, 4)) {
        g_applyDamageOrig(health, damage, attacker, a4, a5, a6, a7, a8, hit);
        return;
    }
    // As MTA: no hit reaction on a remote who is aiming (it would throw their aim off what they really do).
    if (IsPuppetAiming(puppet)) {
        if (g_probe) Log("shots: our hit on puppet 0x%08x while it aims: no reaction", puppet);
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

// For one shot: the gameplay camera (both matrices: rows right, up, back, position) looks from `from` (nullptr = where it
// is) at `to`, and the shooter's reticle point (actor +0xb8 component +0x3800 .. +0x3820) is `to`: a player's shot
// flies from the camera through the reticle point. Put back by Restore.
struct ShotLine {
    float* mats[2] = {};
    float savedCam[2][16] = {};
    float* reticle = nullptr;
    float savedReticle[12] = {};

    ShotLine(uintptr_t actor, const float* from, const float to[3]) {
        const uintptr_t mgr = Ptr(GameAddr(0x142C45738));
        const uintptr_t frame = mgr ? Ptr(mgr + 8) : 0, inst = mgr ? Ptr(mgr + 0x28) : 0;
        mats[0] = frame && IsReadable(frame + 0x1d0, 0x40) ? reinterpret_cast<float*>(frame + 0x1d0) : nullptr;
        mats[1] = inst && IsReadable(inst + 0x10, 0x40) ? reinterpret_cast<float*>(inst + 0x10) : nullptr;
        for (int k = 0; k < 2; ++k) {
            float* m = mats[k];
            if (!m) continue;
            std::memcpy(savedCam[k], m, sizeof(savedCam[k]));
            const float eye[3] = {from ? from[0] : m[12], from ? from[1] : m[13], from ? from[2] : m[14]};
            float b[3] = {eye[0] - to[0], eye[1] - to[1], eye[2] - to[2]};
            const float bl = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (bl < 0.3f) continue;
            for (float& v : b) v /= bl;
            float r[3] = {b[2], 0.0f, -b[0]};
            const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
            if (rl < 1e-4f) continue;
            r[0] /= rl, r[2] /= rl;
            const float u[3] = {b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};
            const float* rows[4] = {r, u, b, eye};
            for (int i = 0; i < 4; ++i) m[i * 4] = rows[i][0], m[i * 4 + 1] = rows[i][1], m[i * 4 + 2] = rows[i][2];
        }
        const uintptr_t comp = actor ? Ptr(actor + 0xb8) : 0;
        if (comp && IsReadable(comp + 0x3800, 0x30)) {
            reticle = reinterpret_cast<float*>(comp + 0x3800);
            std::memcpy(savedReticle, reticle, sizeof(savedReticle));
            for (int i = 0; i < 3; ++i) reticle[i * 4] = to[0], reticle[i * 4 + 1] = to[1], reticle[i * 4 + 2] = to[2];
        }
    }
    ~ShotLine() {
        for (int k = 0; k < 2; ++k)
            if (mats[k]) std::memcpy(mats[k], savedCam[k], sizeof(savedCam[k]));
        if (reticle) std::memcpy(reticle, savedReticle, sizeof(savedReticle));
    }
};

LocalShotFn g_localShot = nullptr;

void ShootHook(uintptr_t weapon, uintptr_t event) {
    const uint32_t owner = WeaponOwner(weapon);
    const uintptr_t info = Ptr(GameAddr(0x1422DAF30));
    const bool local = info && IsReadable(info + 0x5ec, 4) && *reinterpret_cast<uint32_t*>(info + 0x5ec) == owner && !g_firing;
    if (local) {
        if (g_dbgTarget) {
            // [debug] a test copy's player really shoots at the other player (its camera does not turn there): its
            // reticle point is the target for the shot
            ShotLine line(ActorOfHandle(owner), nullptr, g_dbgPoint);
            g_shootOrig(weapon, event);
        } else {
            g_shootOrig(weapon, event);
        }
        if (g_localShot) g_localShot();  // the others get this shot now (bullet sync)
        return;
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

void SetLocalDamageHandler(LocalDamageFn fn) { g_localDamage = fn; }
void SetLocalShotHandler(LocalShotFn fn) { g_localShot = fn; }

void SetBulletSync(uint32_t goh, uint32_t actorHandle, bool on) {
    if (on) g_puppetByActorHandle[actorHandle] = goh;
    else g_puppetByActorHandle.erase(actorHandle);
}

bool FirePuppetShot(uint32_t goh, const Vec3& from, const Vec3& to) {
    if (!g_shootOrig) return false;
    const uintptr_t actor = ActorFromGohHandle(goh);
    const uintptr_t weapon = actor ? WeaponObject(actor) : 0;
    const uintptr_t event = weapon ? FireEvent(weapon) : 0;
    if (!event) {
        if (g_probe) Log("shots: puppet 0x%08x: no weapon object / fire event (%p)", goh, reinterpret_cast<void*>(weapon));
        return false;
    }
    if (*reinterpret_cast<float*>(weapon + 0x12c) < 1.0f) *reinterpret_cast<float*>(weapon + 0x12c) = 1.0f;  // a round for it
    if (g_probe) Log("shots: puppet 0x%08x fires a synced shot (clip %.0f)", goh, *reinterpret_cast<float*>(weapon + 0x12c));
    // the remote's own shot line (their camera -> their crosshair point), as their game fired it (MTA's bullet sync)
    const float f[3] = {from.x, from.y, from.z}, t[3] = {to.x, to.y, to.z};
    const bool haveLine = (from.x != 0.0f || from.z != 0.0f) && (to.x != 0.0f || to.z != 0.0f);
    g_firing = true;
    if (haveLine) {
        ShotLine line(actor, f, t);
        g_shootOrig(weapon, event);
    } else {
        g_shootOrig(weapon, event);
    }
    g_firing = false;
    return true;
}

}  // namespace rdmp
