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
void ForgetPuppetWeapon(uint32_t goh);  // the puppet's actor went away

}  // namespace rdmp
