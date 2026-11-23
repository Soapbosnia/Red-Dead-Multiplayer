#pragma once
// Weapons: the local player's (server actions: give / take / ammo / in hand; the weapon in hand for sync) and remote
// puppets' (armed with the remote's weapon in hand, unlimited ammo; the synced aim / fire inputs make the game's player
// code use it). Ids: shared/weapons.h. Main thread. re/NOTES.md "Weapons".
#include <cstdint>

namespace rdmp {

void InitWeapons(const wchar_t* iniPath);
void WeaponsFrame();  // pending equips; [debug] weapon_probe

// The weapon in the local player's hand (-1 = none / holstered) and its ammo.
void LocalWeapon(int8_t& weapon, uint16_t& ammo);
void GiveLocalWeapon(int weapon, int ammo, bool inHand);
void TakeLocalWeapon(int weapon);  // -1 = every weapon
void SetLocalWeaponAmmo(int weapon, int ammo);
void PutLocalWeaponInHand(int weapon);  // -1 = holster

// Per frame for each streamed-in remote puppet: arm it with the remote's weapon in hand (-1 = holstered).
void SyncPuppetWeapon(uint32_t goh, int weapon);
void ForgetPuppetWeapon(uint32_t goh);
// The local player's aim point (the reticle target point: where the camera's crosshair is in the world).
void LocalAim(float& x, float& y, float& z);
// [debug] aim_hold=N: game copy N sends the aim input held (others see it aim; it does not aim itself). Testing aid.
bool AimHoldDebug();
float AimHoldSide();  // [debug] aim_hold_side: 1 / -1, which side of the camera the held aim point is on
// A remote player's aim point onto their puppet (eased): the puppet's weapon aims there. Main thread, every frame.
void SyncPuppetAim(uint32_t goh, float x, float y, float z, float fromX, float fromY, float fromZ, bool aiming);  // the puppet's actor went away

}  // namespace rdmp
