#pragma once
// Remote players driven by the game's own player code (MTA:SA's context switch, applied to RDR).
//
// MTA makes remote players real player peds and, around each one's CPlayerPed::ProcessControl, puts that player's
// synced controller state into the single global pad. RDR's counterparts (re/NOTES.md "Player input path"):
//   playerInfo  = *(0x1422DAF30), the one local player (+0x5ec its actor, +0x118 its on-foot control object)
//   control     = input mapper object (0x1b90 bytes) that reads every input through vfunc +0x58(id) -> 16-byte record
//                 (*(*(ctrl+0x20)) + id*16: [0] invert mask, [1] timer, [2] value, [3] previous value)
//   FUN_1403726f0(ctrl)        records -> control state (stick, magnitude, desired heading, gait, sprint state)
//   FUN_140bb4150(component)   control state -> the actor's movement request; runs only for playerInfo's actor
// Each attached puppet gets a player-info of its own (a copy of the local one pointing at the puppet), a control
// object of its own whose records come from the network, and the two player-control components the game gives the
// player actor. Around the puppet's control update and its FUN_140bb4150, the global playerInfo points at the
// puppet's, so walking, sprinting, jumping and climbing come out of the same code as for the local player.
#include <cstdint>

namespace rdmp {

constexpr int kInputCount = 30;  // on-foot input ids 0x00..0x1d

// A player's controller state: the effective value of every on-foot input (record [2] ^ [0]; sticks 0x7f = centre)
// and the desired world heading the player's camera gives the stick (control object +0x104, radians).
struct PlayerControls {
    uint8_t value[kInputCount] = {};
    float heading = 0.0f;
    bool valid = false;
};

void InstallPuppetPlayerHooks();

// Gives the puppet player control (once per actor instance; again after a stream-in). False = could not.
bool AttachPlayerControl(uint32_t goh);
void DetachPlayerControl(uint32_t goh);
bool HasPlayerControl(uint32_t goh);
// The controller state the puppet plays from now on.
void SetPlayerControls(uint32_t goh, const PlayerControls& controls);
// The local player's controller state this frame (false before the player exists).
bool CaptureLocalControls(PlayerControls& out);
// Debug (autopilot): the local player's input records and control heading come from `controls` instead of the
// keyboard (nullptr = back to the keyboard).
void SetLocalControlsOverride(const PlayerControls* controls);
// The world point the remote's crosshair is on and where they stand. While the remote aims, the puppet's camera forward
// (player-info +0x30) points from them to it, so the player code turns the puppet that way (aim yaw), and the point is
// its reticle target point (its +0xb8 component +0x3800 / +0x3810 / +0x3820), written after its update (eased).
void SetPlayerAim(uint32_t goh, float x, float y, float z, float fromX, float fromY, float fromZ);
// Main thread, once per frame: runs every attached puppet's control update.
void PuppetPlayerFrame();

}  // namespace rdmp
