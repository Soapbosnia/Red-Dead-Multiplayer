#pragma once
/*
 * RDMP server internals shared by the server's source files:
 *   state.cpp       shared state, the per-player action channel, element ownership, chat, event emission
 *   api_players.cpp player functions (spawn, camera, control, health, killPed, nametags)
 *   api_elements.cpp generic element functions (position, type, lookup, destroy, attach, visibility)
 *   api_blips.cpp   radar blips
 *   api_hud.cpp     chat and engine HUD text
 *   main.cpp        the Framework server instance (lifecycle, connections, death tracking, console)
 *
 * Threads: script callbacks and PostUpdate never run at the same time (the script thread holds the world, see the
 * Framework's script_thread.h); chat arrives on the packet thread. The shared maps are guarded by StateMutex(), always
 * taken AFTER any entity handle (entity lock -> mutex), never before.
 */

#include <integrations/server/instance.h>
#include <logging/logger.h>
#include <networking/network_server.h>
#include <networking/replication/replication_manager.h>
#include <networking/replication/world.h>
#include <scripting/builtins/entity.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "shared/blip.h"
#include "shared/fw_api.h"
#include "shared/player.h"
#include "shared/script_api.h"

namespace RDMP::Server {
    using Framework::Networking::Replication::Ref;
    using Framework::Networking::Replication::World;
    using RDMP::Shared::Blip;
    using RDMP::Shared::Player;
    using Clock = std::chrono::steady_clock;
    using ActionKind = rdmp::fw::ActionKind;
    using ScriptApi::Args;

    // Armadillo Station, the story's first player placement (same default as rdmp.ini).
    constexpr float kSpawnX       = -2208.91f;
    constexpr float kSpawnY       = 16.51f;
    constexpr float kSpawnZ       = 2605.41f;
    constexpr float kSpawnHeading = -100.3f;

    constexpr float kUnlimitedDistance = 16383.0f; // MTA's default visibleDistance

    // ---- the instance and the world ----
    Framework::Integrations::Server::Instance *Instance();
    void SetInstance(Framework::Integrations::Server::Instance *instance);
    std::shared_ptr<spdlog::logger> Log();
    World &GetWorld();
    Framework::Networking::Replication::ReplicationManager *Replication();

    // The resource whose script is calling (empty if none can be named).
    std::string CallingResource(v8::Isolate *isolate);

    // ---- script argument helpers (server elements) ----
    uint64_t ElementId(const Args &args, int i);                  // network id of a Player / Entity handle, 0 if not one
    MafiaNet::PeerGuid PlayerGuid(uint64_t playerId);             // the connection behind a player element
    Ref<Player> PlayerFromArg(const Args &args, int index = 0);
    v8::Local<v8::Value> WrapElement(v8::Isolate *isolate, uint64_t id);
    bool ElementPosition(World &world, uint64_t id, glm::vec3 &out); // players and blips

    // ---- shared state ----
    std::mutex &StateMutex();

    // Elements belong to the resource that created them and go when it stops; blips made for one player only go
    // when that player leaves.
    void OwnElement(uint64_t id, const std::string &resource, MafiaNet::PeerGuid onlyFor);
    void ForgetElement(uint64_t id);
    void ForgetVisibleOnly(uint64_t id);
    void DestroyResourceElements(const std::string &resource);
    void DestroyElementsOnlyFor(MafiaNet::PeerGuid guid);

    // Client-side effects travel as one action at a time on the avatar (Player::act*), acked by the client.
    void QueueAction(uint64_t avatarId, ActionKind kind, float a = 0, float b = 0, float c = 0, float d = 0, const std::string &text = {});
    void FlushActions(World &world); // tick thread
    void ForgetPlayer(uint64_t avatarId);

    // A forced state with a server-authored pose: the owner sees WasPoseForced() and teleports.
    void Teleport(const Ref<Player> &avatar, float x, float y, float z);

    // Deaths, from the client's real health (Player::localHealth) or a scripted kill.
    void MarkDead(uint64_t id);      // scripted kill (setElementHealth 0)
    void MarkRespawned(uint64_t id); // spawnPlayer: re-armed when the client reports a live player again
    // Tick thread: players that just died, and (autoRespawn) players due for the built-in respawn.
    void UpdateDeaths(World &world, bool autoRespawn, std::vector<uint64_t> &wasted, std::vector<uint64_t> &respawned);

    // ---- chat ----
    // A line in chat boxes: to one player's connection, or everyone (guid UNASSIGNED). color 0xRRGGBBAA, 0 = default.
    void SendChat(MafiaNet::PeerGuid to, const std::string &text, const std::string &author = {}, uint32_t colorRGBA = 0,
                  uint64_t senderNetworkId = 0);

    // ---- script events ----
    // Emits a reserved event with the player handle first and optional string arguments; any thread.
    void EmitPlayerEvent(const char *name, uint64_t networkId, std::vector<std::string> extra = {});

    // ---- API modules ----
    void RegisterPlayerApi(const ScriptApi::Publisher &publish);
    void RegisterElementApi(const ScriptApi::Publisher &publish);
    void RegisterBlipApi(const ScriptApi::Publisher &publish);
    void RegisterHudApi(const ScriptApi::Publisher &publish);

    void UpdateAttachedBlips(World &world); // tick thread (api_blips.cpp)
} // namespace RDMP::Server
