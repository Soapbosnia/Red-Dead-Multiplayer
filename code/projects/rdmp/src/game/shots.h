#pragma once
// Bullet sync: a remote player's shots are fired from their puppet here, one bullet per shot their game reports
// (shots.cpp). Main thread.
#include <cstdint>

namespace rdmp {

void InstallShotHooks(bool probe);
// The weapon object of the actor's weapon in hand (clip +0x12c, flags +0x120), 0 = none.
uintptr_t WeaponObject(uintptr_t actor);
// Its fire event (what the fire animation hands the shot function).
uintptr_t FireEvent(uintptr_t weapon);
// The puppet's own fire animation no longer shoots (on): FirePuppetShot does, per remote shot.
void SetBulletSync(uint32_t goh, uint32_t actorHandle, bool on);
// One shot from the puppet's weapon now, the way the fire animation's event would. False: no weapon / event.
bool FirePuppetShot(uint32_t goh);

}  // namespace rdmp
