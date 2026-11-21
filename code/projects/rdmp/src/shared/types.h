#pragma once
// Plain state types shared by rdmp.dll (game) and rdmp_client.dll (framework client). Only POD crosses the
// DLL boundary (see fw_api.h).
#include <cstdint>

namespace rdmp::net {

constexpr uint16_t kDefaultPort = 27015;
constexpr int kMaxPlayers = 64;
constexpr int kNameLen = 32;

// Locomotion desire (game/locomotion.cpp: FUN_140ba6a80's per-frame movement request, 0x110 bytes = 0x44 u32),
// sent sparse: the non-zero u32 words. Known words: [0x14..0x16] local move vector (forward = -1), [0x34] turn,
// [0x35] -1, [0x38] desired world heading in radians (= heading_deg * pi/180 + pi), [0x3a] -1,
// [0x3f] 0xc2280000 | gait, [0x40] 0x1000.
constexpr int kDesireWords = 0x44;
constexpr int kMaxDesireEntries = 20;
struct MoveDesire {
    uint8_t count = 0;
    uint8_t idx[kMaxDesireEntries] = {};
    uint32_t val[kMaxDesireEntries] = {};
};

// Player state. Position in game world units (Y up), heading in degrees (the engine's teleport convention).
struct PlayerState {
    uint32_t seq = 0;
    uint32_t timeMs = 0;  // sender's own clock (ms, wraps) when the game frame captured this state; 0 = unknown
    float x = 0, y = 0, z = 0;
    float heading = 0;
    float speed = 0;      // horizontal m/s
    uint32_t flags = 0;   // low bits: gait
    MoveDesire move;
};

}  // namespace rdmp::net
