#pragma once
#include <cstdint>

namespace rdmp {

// Bytes of the locomotion desire struct (FUN_140ba6a80's param_2 = brain state + 0x240) we capture/override.
constexpr size_t kDesireSize = 0x110;

// Detours the locomotion update FUN_140ba6a80 (called from the per-actor brain update on worker threads).
void InstallLocomotionHook();

// The local player's actor (its desire is captured every frame).
void SetLocoPlayerActor(uintptr_t actor);

// Latest desire produced by the local player's input (false until captured). seq increments per capture.
bool GetPlayerDesire(uint8_t* out, uint32_t* seq);

// Debug: count an actor's locomotion updates and keep its desire word 0x16 (stick forward).
void SetLocoWatchActor(uintptr_t actor);
uint32_t LocoWatchCalls(uint32_t* desire16);

// Debug: address of the local player's desire block (0 until its first locomotion update).
uintptr_t PlayerDesireAddress();

// Debug: the local player's locomotion uses `desire` instead of its input (nullptr = back to the input).
void SetPlayerDesireOverride(const uint8_t* desire);

// The local player's mount (0 = on foot): its desire (what the rider's input asks of the horse) is captured too.
void SetLocoMountActor(uintptr_t actor);
bool GetMountDesire(uint8_t* out, uint32_t* seq);

// From now on, every locomotion update of `actor` uses `desire` instead of what its own brain produced.
void SetPuppetDesire(uintptr_t actor, const uint8_t* desire);
void ClearPuppetDesire(uintptr_t actor);
// Number of times an override was applied (debug).
uint32_t LocoOverridesApplied();

}  // namespace rdmp
