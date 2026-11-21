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

// From now on, every locomotion update of `actor` uses `desire` instead of what its own brain produced.
void SetPuppetDesire(uintptr_t actor, const uint8_t* desire);
void ClearPuppetDesire(uintptr_t actor);
// Number of times an override was applied (debug).
uint32_t LocoOverridesApplied();

}  // namespace rdmp
