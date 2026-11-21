#pragma once
// The server's vehicles in the game (shared/vehicle.h: horses). Main thread, once per frame from the net client.
//
// Every vehicle within stream range gets a horse actor. The one the local player rides (and owns) is moved by the
// game from the player's own input; its pose and that input go to the server. Every other one is driven from its
// network state by the puppet controller: the rider's input when ridden, standing still when not (no animal AI: our
// movement request replaces the horse brain's every frame).
#include <cstdint>

#include "actors.h"
#include "shared/fw_api.h"

namespace rdmp {

void LoadVehicleConfig(const wchar_t* ini);

// One frame: the replicated set from the client DLL, the local player's position (streaming), the time, the
// interpolation buffer and the network delay of the shown position.
void VehiclesFrame(const fw::Api& api, const Vec3& localPos, double nowMs, uint32_t interpMs, float leadMs);

// The vehicle the game has the local player on (0 = on foot), for the player's state.
uint64_t LocalVehicle();
// The horse actor (goh handle) of a vehicle, 0 when it has none here.
uint32_t VehicleGoh(uint64_t netId);

// Rider natives (goh handles): ACTOR_MOUNT_ACTOR / ACTOR_DISMOUNT_NOW / GET_MOUNT.
void MountActor(uint32_t riderGoh, uint32_t horseGoh);
void DismountActor(uint32_t riderGoh);
uint32_t MountOf(uint32_t riderGoh);

}  // namespace rdmp
