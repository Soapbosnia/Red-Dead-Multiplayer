#pragma once
// Weapons: the local player's (server actions: give / take / ammo / in hand; the weapon in hand for sync) and remote
// puppets' (armed with the remote's weapon in hand, unlimited ammo; the synced aim / fire inputs make the game's player
// code use it). Ids: shared/weapons.h. Main thread. re/NOTES.md "Weapons".
#include <cstdint>

#include "actors.h"
#include "puppet_player.h"

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
// The local player's weapon state, from their own game (aim state, weapon-ready time, drawn, reloading, clip; a shot is
// counted when the clip goes down outside a reload).
void LocalWeaponState(WeaponState& out);
// A remote's weapon state onto their puppet, every frame (main thread): the state itself (puppet_player.cpp) and the
// puppet's clip (the remote's rounds plus the shots it still has to fire, so it fires and reloads when they do).
// Bullet sync: each new shot is fired from the puppet along the remote's shot line (camera position -> crosshair).
void SyncPuppetWeaponState(uint32_t goh, int weapon, const WeaponState& state, const Vec3& shotFrom, const Vec3& shotTo);
// The local camera's position (where the local player's shots start).
void LocalCamera(float& x, float& y, float& z);
// Damage the local player took since the last call (health drop): who hit them (the attacker's actor, a goh handle; 0 =
// none), with what (shared/weapons.h, -1 = none), where (the game's hit zone, -1 = unknown) and how much. The game's own
// record of the hit (GET_LAST_ATTACKER and co.) is cleared after reading.
struct LocalHit {
    uint32_t attacker = 0;
    int weapon = -1;
    int bodyPart = -1;
    float loss = 0.0f;
    float health = 0.0f;  // left after the hit
    float before = 0.0f;  // before it
};
bool PollLocalHit(float health, LocalHit& out);
// A script sets the local player's health: the drop to it is not damage (no clientPlayerDamage / playerDamage).
void ExpectLocalHealth(float health);
// The local player's aim point (the reticle target point: where the camera's crosshair is in the world).
void LocalAim(float& x, float& y, float& z);
// [debug] aim_hold=N: game copy N holds its aim input (its player aims, no mouse button needed). Two-copy tests.
bool AimHoldDebug();
bool AimHoldAtRemote();  // [debug] aim_hold_target=1: the held aim point is the first remote player's chest
void FaceLocalPlayerAt(const Vec3& from, const Vec3& to);  // [debug] aim_hold_target: the local player turns to face it
// [debug] aim_hold_point=1: the point that copy sends instead of its crosshair: 20 m away at a world yaw (aim_hold_yaw,
// degrees) turning at aim_hold_sweep deg/s, up / down by aim_hold_pitch degrees (swinging when aim_hold_pitch_swing=1).
void AimHoldPoint(const Vec3& pos, double nowMs, float& x, float& y, float& z);
// A remote player's aim, every frame (main thread): their aim point and where they stand (puppet_player SetPlayerAim:
// the puppet's camera looks from them to it) and, while they aim, their body heading (degrees, interpolated): the
// local player's camera turns its body as it aims, a puppet has no camera of its own, so it gets the remote's turn.
void SyncPuppetAim(uint32_t goh, const Vec3& aim, const Vec3& from, float bodyHeading, bool aiming);

}  // namespace rdmp
