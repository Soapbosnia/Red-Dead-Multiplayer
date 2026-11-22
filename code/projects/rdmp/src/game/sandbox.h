#pragma once
#include <windows.h>

#include <cstdint>

#include "actors.h"

namespace rdmp {

// Sandbox boot (rdmp.ini [sandbox]): with root_script = sp_idle, finish the loading screen automatically
// and spawn only the player.
void LoadSandboxConfig(const wchar_t* iniPath);

// Called from the UI event hook on the game's main thread, after the event has been delivered.
void SandboxOnUiEvent(const char* name);

// Direct start of the world load (map init, loader setup, new-game flag). Main thread only.
void StartGameDirect();
// Marks the sandbox as in single-player (what StartGameDirect / "enterSinglePlayer" do), for our own boot.
void SandboxMarkInGame();
// Goh handle of the local player body (0 = none).
uint32_t SandboxPlayerGoh();

// Called when the in-game world load starts (root script requested by FUN_1401ee430).
void SandboxOnWorldLoadStarted();

// Kills every running script thread (sp_idle otherwise keeps driving scenic cameras).
void KillScriptThreads(const char* why);

// Remote-player prototype: spawns a second (non-player) Marston 3 m from the player. Main thread only.
void SpawnPuppetNearPlayer();

// Prototype: toggles puppets circling the player (direct per-frame transform writes on the main thread).
void TogglePuppetOrbit();

// Prototype: orders every puppet to walk to the player's position (TASK_GO_TO_COORD). Main thread only.
void PuppetsComeToPlayer();

// Resets the game camera and cuts it behind the player (what the story's end-of-load transitions do).
void CameraToPlayer(const char* why);

// Network client API (main thread only).
// Local player position (Y up) and heading in degrees; false until the sandbox player exists.
bool GetLocalPlayerState(Vec3& pos, float& heading);
// Actor handle of the local player (what script natives take as an Actor), 0 = none.
uint32_t LocalPlayerActorHandle();
// True once the world load finished (the net client connects then, even before a player exists).
bool IsWorldReady();
// Local player health (*(actor+0x60)+0x20; 100 alive, <= 0 dead); false until the player exists.
bool GetLocalPlayerHealth(float& health);
// Engine teleport of the local player (server override, e.g. the server console "tp").
bool TeleportLocalPlayer(const Vec3& pos, float heading);
// Spawns an NPC actor of an actor definition (e.g. a horse, 976..1005); returns its goh handle (0 = failed).
uint32_t SpawnActorOfDef(uint32_t def, int32_t variation, const Vec3& pos, float heading, const char* label);
// Spawns a remote-player puppet (rdmp.ini puppet_def, default 0 = Marston model); returns its goh handle (0 = failed).
uint32_t SpawnRemotePuppet(const Vec3& pos, float heading, const char* label);
// Engine teleport of a puppet (FUN_140ad4ca0).
bool SetPuppetTransform(uint32_t goh, const Vec3& pos, float heading);
// Same without the warp reset of the motion component (keeps the locomotion running): small per-frame corrections.
bool NudgePuppet(uint32_t goh, const Vec3& pos, float heading);
// goh handle -> actor entry (the 0x142C457F8 table object) / its actor handle; 0 = gone.
uintptr_t ActorFromGohHandle(uint32_t gohHandle);
uint32_t ActorHandleFromGohHandle(uint32_t gohHandle);
// Puppet actor, with an AI brain created if missing (needed for the locomotion override). 0 = gone.
uintptr_t PreparePuppetLocomotion(uint32_t goh);
bool GetPuppetPosition(uint32_t goh, Vec3& pos);
bool GetPuppetPose(uint32_t goh, Vec3& pos, float& headingDeg);
// Distance streaming of remote players: release / re-create the puppet's actor instance (goh Actor vf2 / vf0).
bool StreamOutPuppet(uint32_t goh);
bool StreamInPuppet(uint32_t goh, const Vec3& pos, float heading);

// Script natives: server-requested client actions. Safe from any thread (marshalled to the main thread).
void ScriptSpawnPlayer(const Vec3& pos, float heading);
void ScriptFadeCamera(bool fadeIn, float milliseconds);
void ScriptSetCameraTarget();
void ScriptSetPlayerControl(bool enabled);
void ScriptSetPlayerHealth(float health);
// Main thread: our UI (chat input) holds the keyboard; player control stays off until released.
void SetUiInputLock(bool locked);
// The connection to the server ended: limbo (no control, invulnerable) until the next spawnPlayer, the main menu takes
// over; fadeToBlack fades the screen out first (HTML menu).
// fadeToBlack = false (native menus): no engine fade - it brings the UI's FadeToBlack scene, which ends the pause (the
// menu the player left from) and holds the UI stack until a fade-in; the player stays where it is, in limbo behind the menu.
void ScriptLeaveSession(bool fadeToBlack);

// Sets the loader's "finish loading" request bit (the frame update acts on it once the world is ready).
void RequestFinishLoading(const char* why);

}  // namespace rdmp
