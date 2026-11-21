#pragma once
// Plain state types shared by rdmp.dll (game) and rdmp_client.dll (framework client). Only POD crosses the
// DLL boundary (see fw_api.h).
#include <cstdint>

namespace rdmp::net {

constexpr uint16_t kDefaultPort = 27015;
constexpr int kMaxPlayers = 64;
constexpr int kNameLen = 32;

// Locomotion desire (game/locomotion.cpp: FUN_140ba6a80's per-frame movement request, 0x110 bytes = 0x44 u32).
// Known words: [0x14..0x16] local move vector (forward = -1), [0x34] turn, [0x35] -1, [0x38] desired world heading
// in radians (= heading_deg * pi/180 + pi), [0x3a] -1, [0x3f] 0xc2280000 | gait, [0x40] 0x1000. These are all the
// words a player on foot produces (captured walking, running, sprinting, turning).
constexpr int kDesireWords = 0x44;

// A player's movement input: the part of the desire a puppet needs (the rest is constant or recomputed). Travels
// with the position in every transform update, so input and position of one sample always belong together (MTA's
// pure sync does the same with its key states).
struct MoveInput {
    float right = 0;    // [0x14] stick, local right (-1..1)
    float back = 0;     // [0x16] stick, local backward (-1..1; walking forward = -1)
    float heading = 0;  // [0x38] desired world heading + pi (rad, 0..2pi); only meaningful while the stick is pressed
    uint8_t gait = 0;   // low byte of [0x3f]: 0 standing, 1 walk, 4 sprint
};

// Player state. Position in game world units (Y up), heading in degrees (the engine's teleport convention).
struct PlayerState {
    uint32_t seq = 0;
    uint32_t timeMs = 0;  // sender's own clock (ms, wraps) when the game frame captured this state; 0 = unknown
    float x = 0, y = 0, z = 0;
    float heading = 0;
    float speed = 0;      // horizontal m/s
    MoveInput input;
    // The player's controller state: the effective value of every on-foot input (game/puppet_player.h; sticks 0x7f =
    // centre). Puppets in player mode are driven by the game's own player code from these.
    static constexpr int kControlCount = 30;
    uint8_t controls[kControlCount] = {};
    // Local: the vehicle the game has the player on. Remote: the vehicle the server has them on. 0 = on foot.
    uint64_t vehicle = 0;
};

}  // namespace rdmp::net
