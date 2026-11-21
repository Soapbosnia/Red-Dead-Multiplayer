#pragma once
// Radar / pause-map blips driven by the server (createBlip / createBlipAttachedTo ...).
//
// The server replicates one RDMP::Blip entity per script blip (shared/blip.h); rdmp_client.dll
// hands the current set to rdmp.dll (fw_api get_blips) and ApplyBlips() reconciles the engine's blips with it
// every frame on the main thread: create what is new, update what changed, remove what is gone. State, not
// events: a lost packet, a late join or an engine-side invalidation all converge on the next frame.
#include <cstdint>

#include "actors.h"  // Vec3

namespace rdmp {

struct WantedBlip {
    uint64_t id = 0;          // server entity id
    Vec3 pos{};               // world position (Y up); for attached blips the server's last known position
    uint64_t attachedTo = 0;  // network id of the element it follows (0 = fixed position)
    uint16_t icon = 0;        // BlipIcon (299..572)
    uint32_t rgba = 0xFFFFFFFF;
    float scale = 1.0f;
    float maxDistance = -1.0f;  // < 0 = unlimited
    int16_t priority = 0;
    uint8_t flags = 0;          // fw::BlipFlags
};

// Resolves the live position of an element by network id (the local player or a streamed remote player).
using ElementPositionFn = bool (*)(uint64_t netId, Vec3& out, void* user);

// Main thread only. `count` = 0 removes every blip the mod created (disconnect).
void ApplyBlips(const WantedBlip* wanted, int count, ElementPositionFn resolve, void* user);

// Number of engine blips currently created by the mod.
int LiveBlipCount();

}  // namespace rdmp
