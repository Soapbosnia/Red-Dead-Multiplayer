#pragma once
#include <cstdint>

namespace rdmp {

struct Vec3 { float x, y, z; };

// rdmp.ini [debug] trace_after_player: trace the 600 native calls after a script creates the "player" actor.
extern bool g_traceAfterPlayerCreate;

// Installs the logging replacement for the script "create actor" native (FUN_140232050).
void InstallActorHooks();

// Calls the game's create-actor routine (FUN_1402211a0) like the script native does.
// Must run on the game's main thread. Returns the actor handle (0 = failed).
uint32_t CreateActor(uint32_t a0, uint64_t a1, uint32_t a2, const Vec3& pos, const Vec3& rot, int32_t a7);

// Calls the factory FUN_1402209e0 directly (bypasses the playerInfo+0x5e9 gate in FUN_1402211a0).
uint32_t CreateActorDirect(uint32_t owner, uint64_t name, uint32_t def, const Vec3& pos, const Vec3& rot, int32_t variation, uint8_t asPlayer = 0);

}  // namespace rdmp
