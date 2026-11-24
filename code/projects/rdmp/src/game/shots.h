#pragma once
// Bullet sync: a remote player's shots are fired from their puppet here, one bullet per shot their game reports
// (shots.cpp). Main thread.
#include <cstdint>

#include "actors.h"  // Vec3

namespace rdmp {

void InstallShotHooks(bool probe);
// The weapon object of the actor's weapon in hand (clip +0x12c, flags +0x120), 0 = none.
uintptr_t WeaponObject(uintptr_t actor);
// Its fire event (what the fire animation hands the shot function).
uintptr_t FireEvent(uintptr_t weapon);
// The puppet's own fire animation no longer shoots (on): FirePuppetShot does, per remote shot.
void SetBulletSync(uint32_t goh, uint32_t actorHandle, bool on);
// One shot from the puppet's weapon now, the way the fire animation's event would. False: no weapon / event.
bool FirePuppetShot(uint32_t goh, const Vec3& from, const Vec3& to);
// [debug] aim_hold_target: the local player's own shots go at this point (a test copy's camera does not turn there).
void SetDebugShotTarget(bool on, float x, float y, float z);
// The local player is about to take a hit (the game's damage function, before it applies it): attacker = the actor
// handle that hit them (0 = none), weapon (shared/weapons.h), the hit zone (-1 = unknown), the damage and the health
// before it. False = cancel the hit (nothing applied, lethal hits too).
using LocalDamageFn = bool (*)(uint32_t attacker, int weapon, int zone, float damage, float health);
void SetLocalDamageHandler(LocalDamageFn fn);

}  // namespace rdmp
