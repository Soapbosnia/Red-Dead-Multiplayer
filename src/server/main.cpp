/*
 * RDMP (Red Dead Multiplayer) dedicated server, on the MafiaHub Framework 44.
 *
 * One avatar entity per connection, owned by that connection and used as its viewer, so a
 * player's position decides what the server streams to them. Script natives (MTA-style) are
 * published to every resource context; client-side effects travel as queued actions on the
 * avatar (acked by the client, see player.h), script blips as server-owned RDMP::Blip
 * entities (blip.h).
 *
 * Threads: script callbacks and PostUpdate never run at the same time (the script thread holds
 * the world, see script_thread.h); chat commands arrive on the packet thread. Shared maps below are
 * guarded by g_stateMutex, always taken AFTER any entity handle (entity lock -> mutex), never before.
 */

#include <integrations/server/instance.h>
#include <logging/logger.h>
#include <networking/network_server.h>
#include <networking/replication/entity_registry.h>
#include <networking/replication/replication_manager.h>
#include <networking/replication/world.h>
#include <utils/version.h>

#include <scripting/builtins/entity.h>
#include <scripting/engine_scope.h>
#include <scripting/resource/resource.h>
#include <scripting/resource/resource_manager.h>
#include <v8pp/convert.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "shared/blip_icons.h"
#include "shared/blip.h"
#include "shared/player.h"

namespace {
    using Framework::Networking::Replication::EntityRegistry;
    using Framework::Networking::Replication::Ref;
    using Framework::Networking::Replication::World;
    using RDMP::Shared::Blip;
    using RDMP::Shared::Player;
    using Clock = std::chrono::steady_clock;
    namespace Builtins = Framework::Scripting::Builtins;

    // Armadillo Station, the story's first player placement (same default as rdmp.ini).
    constexpr float kSpawnX       = -2208.91f;
    constexpr float kSpawnY       = 16.51f;
    constexpr float kSpawnZ       = 2605.41f;
    constexpr float kSpawnHeading = -100.3f;

    constexpr auto kRespawnDelay      = std::chrono::seconds(5);         // built-in respawn (RDMP_AUTO_RESPAWN=1)
    constexpr auto kActionResend      = std::chrono::milliseconds(300);  // un-acked action: push the forced state again
    constexpr int kActionMaxResends   = 10;                              // then drop it (client gone or wedged)
    constexpr auto kAttachInterval    = std::chrono::milliseconds(100);  // attached blips follow their element at 10 Hz
    constexpr size_t kMaxBlips        = 1024;                            // server-wide; a client shows at most 120
    constexpr float kUnlimitedDistance = 16383.0f;                       // MTA's default visibleDistance

    Framework::Integrations::Server::Instance *gInstance = nullptr;
    void EmitPlayerEventFwd(const char *name, uint64_t networkId);

    std::shared_ptr<spdlog::logger> Log() {
        return Framework::Logging::GetLogger(FRAMEWORK_INNER_SERVER);
    }

    World &GetWorld() {
        return gInstance->GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager()->GetWorld();
    }

    Framework::Networking::Replication::ReplicationManager *Replication() {
        return gInstance->GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager();
    }

    // The resource whose script is calling a native (empty if none can be named).
    std::string CallingResource(v8::Isolate *isolate) {
        auto *module   = gInstance->GetScriptingModule();
        auto *manager  = module ? module->GetResourceManager() : nullptr;
        auto *resource = manager ? manager->GetCurrentResourceWithStackFallback(isolate) : nullptr;
        return resource ? resource->GetName() : std::string();
    }

    void Reply(uint64_t networkId, const std::string &line) {
        // Player-facing chat would go through Framework::Networking::RPC::ChatMessage; until then the
        // server console is the visible output.
        (void)networkId;
        Log()->info("[cmd] {}", line);
    }

    // ---- shared state (g_stateMutex) ------------------------------------------------------------
    std::mutex g_stateMutex;

    // Client-side natives travel as one action at a time on the avatar (actSeq/actKind/actA..D, server
    // fields pushed by ForceState). The next one is sent only after the client echoed actAck == actSeq:
    // the entity has a single slot, so two actions in flight would overwrite each other.
    struct PendingAction {
        uint8_t kind;
        float a, b, c, d;
    };
    struct ActionChannel {
        std::deque<PendingAction> queue;
        uint8_t inFlight = 0; // seq waiting for its ack, 0 = none
        Clock::time_point sentAt {};
        int resends = 0;
    };
    std::map<uint64_t, ActionChannel> g_actions;
    uint8_t g_actionSeq = 0;

    // Death tracking from the client's real health (Player::localHealth). `armed` once the player has been
    // seen alive since the last death, so the stale 0 that lingers until the respawn cannot kill twice.
    struct DeathState {
        bool armed = false;
        bool dead  = false;
        Clock::time_point diedAt {};
    };
    std::map<uint64_t, DeathState> g_death;

    bool g_autoRespawn = false; // RDMP_AUTO_RESPAWN=1: respawn at Armadillo 5 s after death (no gamemode needed)

    // Elements a resource created (MTA: they belong to the resource and go when it stops), and the players a blip
    // was made visible to only (destroyed once none of them is connected any more).
    std::unordered_map<uint64_t, std::string> g_elementResource;
    std::unordered_map<uint64_t, std::vector<MafiaNet::PeerGuid>> g_elementOnlyFor;

    void ForgetElement(uint64_t id) {
        std::lock_guard lock(g_stateMutex);
        g_elementResource.erase(id);
        g_elementOnlyFor.erase(id);
    }

    void QueueAction(uint64_t avatarId, uint8_t kind, float a, float b, float c, float d) {
        std::lock_guard lock(g_stateMutex);
        g_actions[avatarId].queue.push_back({kind, a, b, c, d});
    }

    uint8_t NextActionSeq() {
        if (++g_actionSeq == 0) {
            g_actionSeq = 1; // 0 means "never sent"
        }
        return g_actionSeq;
    }

    // Tick thread (PostUpdate).
    void FlushActions(World &world) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard lock(g_stateMutex);
            for (const auto &[id, channel] : g_actions) {
                ids.push_back(id);
            }
        }
        const auto now = Clock::now();
        for (const uint64_t id : ids) {
            auto avatar = world.Write<Player>(id);
            std::lock_guard lock(g_stateMutex);
            auto it = g_actions.find(id);
            if (it == g_actions.end()) {
                continue;
            }
            ActionChannel &ch = it->second;
            if (!avatar) {
                g_actions.erase(it);
                continue;
            }
            if (ch.inFlight != 0) {
                if (avatar->actAck == ch.inFlight) {
                    ch.inFlight = 0; // delivered
                }
                else if (now - ch.sentAt >= kActionResend) {
                    if (++ch.resends > kActionMaxResends) {
                        Log()->warn("{}: action {} (kind {}) never acked, dropped", avatar->nickname, ch.inFlight, avatar->actKind);
                        ch.inFlight = 0;
                    }
                    else {
                        avatar->ForceState(); // same seq again; the client ignores a seq it has already taken
                        ch.sentAt = now;
                    }
                }
                if (ch.inFlight != 0) {
                    continue;
                }
            }
            if (ch.queue.empty()) {
                g_actions.erase(it);
                continue;
            }
            const PendingAction action = ch.queue.front();
            ch.queue.pop_front();
            avatar->actSeq  = NextActionSeq();
            avatar->actKind = action.kind;
            avatar->actA    = action.a;
            avatar->actB    = action.b;
            avatar->actC    = action.c;
            avatar->actD    = action.d;
            avatar->ForceState();
            ch.inFlight = avatar->actSeq;
            ch.sentAt   = now;
            ch.resends  = 0;
        }
    }

    // A forced state with a server-authored pose: the owner sees WasPoseForced() and teleports.
    void Teleport(const Ref<Player> &avatar, float x, float y, float z) {
        avatar->position = {x, y, z};
        avatar->speed    = 0.0f;
        avatar->ForceState();
    }

    // ---- script argument helpers ------------------------------------------------------------------
    void Throw(v8::Isolate *isolate, const char *usage) {
        isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8(isolate, usage).ToLocalChecked()));
    }

    bool Has(const v8::FunctionCallbackInfo<v8::Value> &args, int i) {
        return args.Length() > i && !args[i]->IsUndefined() && !args[i]->IsNull();
    }

    double Num(const v8::FunctionCallbackInfo<v8::Value> &args, int i, double fallback) {
        if (!Has(args, i) || !args[i]->IsNumber()) {
            return fallback;
        }
        return args[i].As<v8::Number>()->Value();
    }

    // Network id of a script element handle (Player or Entity builtin), 0 if the value is not one.
    uint64_t ElementId(const v8::FunctionCallbackInfo<v8::Value> &args, int i) {
        if (!Has(args, i) || !args[i]->IsObject()) {
            return 0;
        }
        auto *handle = v8pp::from_v8<Builtins::Entity *>(args.GetIsolate(), args[i]);
        return handle ? handle->GetId() : 0;
    }

    // The connection a player element belongs to (UNASSIGNED if the element is not a player).
    MafiaNet::PeerGuid PlayerGuid(uint64_t playerId) {
        MafiaNet::PeerGuid guid = MafiaNet::UNASSIGNED_PEER_GUID;
        if (playerId && GetWorld().Is<Player>(playerId)) {
            Builtins::Entity(playerId).ResolveOwner(guid);
        }
        return guid;
    }

    Ref<Player> PlayerFromArg(const v8::FunctionCallbackInfo<v8::Value> &args, int index = 0) {
        const uint64_t id = ElementId(args, index);
        return id ? GetWorld().Write<Player>(id) : Ref<Player>();
    }

    v8::Local<v8::Value> WrapElement(v8::Isolate *isolate, uint64_t id) {
        if (!id) {
            return v8::Null(isolate);
        }
        if (GetWorld().Is<Player>(id)) {
            return gInstance->WrapScriptPlayer(isolate, id);
        }
        return gInstance->WrapScriptEntity(isolate, id);
    }

    // Element position for any element type the server knows (players and blips).
    bool ElementPosition(World &world, uint64_t id, glm::vec3 &out) {
        if (auto p = world.Read<Player>(id)) {
            out = p->position;
            return true;
        }
        if (auto b = world.Read<Blip>(id)) {
            out = b->position;
            return true;
        }
        return false;
    }

    // ---- player natives ---------------------------------------------------------------------------
    //
    //   spawnPlayer(player, x, y, z, heading)   create / respawn / move the player
    //   fadeCamera(player, fadeIn, ms)          engine fader on that player's client
    //   setCameraTarget(player)                 attach the gameplay camera to the player
    //   setElementControl(player, enabled)      player control on/off
    //   setElementHealth(player, health)        health (0 = scripted kill, see below)
    void SpawnPlayerCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        uint64_t id   = 0;
        {
            auto player = PlayerFromArg(args);
            if (!player || args.Length() < 4) {
                Throw(isolate, "spawnPlayer(player, x, y, z[, heading])");
                return;
            }
            const float x       = static_cast<float>(Num(args, 1, 0.0));
            const float y       = static_cast<float>(Num(args, 2, 0.0));
            const float z       = static_cast<float>(Num(args, 3, 0.0));
            const float heading = static_cast<float>(Num(args, 4, 0.0));
            player->flags  = static_cast<uint8_t>(player->flags | Player::kAlive);
            player->health = 100;
            id             = player.Id();
            QueueAction(id, 1 /* Spawn */, x, y, z, heading);
            std::lock_guard lock(g_stateMutex);
            g_death[id].dead = false; // re-armed when the client reports a live player again
        }
        EmitPlayerEventFwd("playerSpawned", id); // MTA's onPlayerSpawn
    }

    void FadeCameraCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        auto player   = PlayerFromArg(args);
        if (!player) {
            Throw(isolate, "fadeCamera(player, fadeIn, ms)");
            return;
        }
        const bool fadeIn = args.Length() > 1 ? args[1]->BooleanValue(isolate) : true;
        const float ms    = static_cast<float>(Num(args, 2, 1000.0));
        QueueAction(player.Id(), 2 /* Fade */, fadeIn ? 1.0f : 0.0f, ms, 0.0f, 0.0f);
    }

    void SetControlCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        auto player   = PlayerFromArg(args);
        if (!player) {
            Throw(isolate, "setElementControl(player, enabled)");
            return;
        }
        const bool enabled = args.Length() > 1 ? args[1]->BooleanValue(isolate) : true;
        QueueAction(player.Id(), 4 /* Control */, enabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // health 0 is a scripted kill: RDR1 health regenerates and the client never writes a real 0 for it (the
    // game side clamps to 1 so the engine keeps the actor), so the server performs the death itself.
    void SetHealthCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        uint64_t killed = 0;
        {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(isolate, "setElementHealth(player, health)");
                return;
            }
            const float health = std::clamp(static_cast<float>(Num(args, 1, 100.0)), 0.0f, 255.0f);
            player->health     = static_cast<uint8_t>(health);
            QueueAction(player.Id(), 5 /* Health */, health, 0.0f, 0.0f, 0.0f);
            if (player->health == 0 && (player->flags & Player::kAlive)) {
                player->flags = static_cast<uint8_t>(player->flags & ~Player::kAlive);
                killed        = player.Id();
                Log()->info("{} wasted via script at ({:.1f}, {:.1f}, {:.1f})", player->nickname, player->position.x,
                            player->position.y, player->position.z);
                std::lock_guard lock(g_stateMutex);
                DeathState &d = g_death[killed];
                d.armed       = false;
                d.dead        = true;
                d.diedAt      = Clock::now();
            }
            else if (player->health > 0) {
                player->flags = static_cast<uint8_t>(player->flags | Player::kAlive);
            }
        }
        if (killed) {
            EmitPlayerEventFwd("playerWasted", killed);
        }
    }

    void SetCameraTargetCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        auto player   = PlayerFromArg(args);
        if (!player) {
            Throw(isolate, "setCameraTarget(player)");
            return;
        }
        QueueAction(player.Id(), 3 /* CameraTarget */, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // getElementHealth(player): the game's real health as the client last reported it.
    void GetHealthCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto player = PlayerFromArg(args);
        args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), player ? player->localHealth : 0));
    }

    void GetNameCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        auto player   = PlayerFromArg(args);
        const std::string name = player ? player->nickname : std::string();
        args.GetReturnValue().Set(v8::String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked());
    }

    // ---- generic element natives (players + blips) --------------------------------------------
    //
    //   getElementPosition(element) -> {x, y, z}       setElementPosition(element, x, y, z)
    //   getElementType(element) -> "player"|"blip"|false   isElement(element)
    //   getElementsByType("player"|"blip") -> [elements]
    //   destroyElement(blip)                            attachElements(blip, target) / detachElements(blip)
    //   getElementAttachedTo(blip) -> element|null      setElementVisibleTo(blip, player|null)
    void GetPositionCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        glm::vec3 p {};
        const uint64_t id = ElementId(args, 0);
        if (!id || !ElementPosition(GetWorld(), id, p)) {
            Throw(isolate, "getElementPosition(element) -> {x,y,z}");
            return;
        }
        auto context = isolate->GetCurrentContext();
        auto result  = v8::Object::New(isolate);
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "x"), v8::Number::New(isolate, p.x)).Check();
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "y"), v8::Number::New(isolate, p.y)).Check();
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "z"), v8::Number::New(isolate, p.z)).Check();
        args.GetReturnValue().Set(result);
    }

    void SetPositionCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        if (!id || args.Length() < 4) {
            Throw(isolate, "setElementPosition(element, x, y, z)");
            return;
        }
        const float x = static_cast<float>(Num(args, 1, 0.0));
        const float y = static_cast<float>(Num(args, 2, 0.0));
        const float z = static_cast<float>(Num(args, 3, 0.0));
        auto &world   = GetWorld();
        if (auto player = world.Write<Player>(id)) {
            Teleport(player, x, y, z); // the client sees a server-authored pose and moves the real player
            args.GetReturnValue().Set(true);
            return;
        }
        if (auto blip = world.Write<Blip>(id)) {
            blip->position = {x, y, z}; // an attached blip keeps following its element (MTA does the same)
            args.GetReturnValue().Set(true);
            return;
        }
        args.GetReturnValue().Set(false);
    }

    void GetTypeCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        auto &world       = GetWorld();
        if (id && world.Is<Player>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "player"));
        }
        else if (id && world.Is<Blip>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "blip"));
        }
        else {
            args.GetReturnValue().Set(false);
        }
    }

    void IsElementCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t id = ElementId(args, 0);
        args.GetReturnValue().Set(id != 0 && GetWorld().Exists(id));
    }

    void GetElementsByTypeCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString()) {
            Throw(isolate, "getElementsByType(\"player\"|\"blip\") -> [elements]");
            return;
        }
        const std::string type = *v8::String::Utf8Value(isolate, args[0]);
        std::vector<uint64_t> ids;
        if (type == "player") {
            ids = GetWorld().IdsOf<Player>();
        }
        else if (type == "blip") {
            ids = GetWorld().IdsOf<Blip>();
        }
        auto context = isolate->GetCurrentContext();
        auto result  = v8::Array::New(isolate, static_cast<int>(ids.size()));
        for (uint32_t i = 0; i < ids.size(); ++i) {
            result->Set(context, i, WrapElement(isolate, ids[i])).Check();
        }
        args.GetReturnValue().Set(result);
    }

    // ---- blips ------------------------------------------------------------------------------------
    //
    //   createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
    //   createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
    //   get/setBlipIcon, get/setBlipSize, get/setBlipColor, get/setBlipOrdering, get/setBlipVisibleDistance,
    //   setBlipVisible(blip, bool), setBlipDisplay(blip, "both"|"radar"|"map")
    //
    // MTA argument order. Differences: icon is an RDR BlipIcon (number 299..572 or a name such as
    // "BLIP_OBJECTIVE" / "objective"; BlipIcon.* holds them all); size maps to the engine scale as size / 2
    // (MTA's default 2 = scale 1); the colour is a tint applied only when given (RDR icons carry their own
    // colours); visibleDistance >= 16383 (MTA's default) means unlimited.
    uint16_t IconArg(const v8::FunctionCallbackInfo<v8::Value> &args, int i, uint16_t fallback) {
        if (!Has(args, i)) {
            return fallback;
        }
        if (args[i]->IsString()) {
            const std::string name = *v8::String::Utf8Value(args.GetIsolate(), args[i]);
            const uint16_t id      = RDMP::Shared::BlipIconFromName(name.c_str());
            return id ? id : fallback;
        }
        const double v = Num(args, i, fallback);
        return (v >= Blip::kIconMin && v <= Blip::kIconMax) ? static_cast<uint16_t>(v) : fallback;
    }

    uint8_t ColorByte(const v8::FunctionCallbackInfo<v8::Value> &args, int i, double fallback) {
        return static_cast<uint8_t>(std::clamp(Num(args, i, fallback), 0.0, 255.0));
    }

    float DistanceArg(double v) {
        return (v <= 0.0 || v >= kUnlimitedDistance) ? -1.0f : static_cast<float>(v);
    }

    // Shared tail of createBlip / createBlipAttachedTo: args[first..] = icon, size, r, g, b, a, ordering,
    // visibleDistance, visibleTo.
    void CreateBlipCommon(const v8::FunctionCallbackInfo<v8::Value> &args, int first, const glm::vec3 &pos, uint64_t attachedTo) {
        auto *isolate = args.GetIsolate();
        auto &world   = GetWorld();
        if (world.IdsOf<Blip>().size() >= kMaxBlips) {
            Log()->warn("createBlip: the server already has {} blips", kMaxBlips);
            args.GetReturnValue().Set(false);
            return;
        }
        const MafiaNet::PeerGuid visibleTo = PlayerGuid(ElementId(args, first + 8));
        const std::string resource         = CallingResource(isolate);
        uint64_t id                        = 0;
        {
            auto blip = world.Create<Blip>();
            if (!blip) {
                args.GetReturnValue().Set(false);
                return;
            }
            blip->position    = pos;
            blip->icon        = IconArg(args, first, Blip::kDefaultIcon);
            blip->scale       = static_cast<float>(std::clamp(Num(args, first + 1, 2.0), 0.0, 50.0) / 2.0);
            const bool colour = Has(args, first + 2) || Has(args, first + 3) || Has(args, first + 4) || Has(args, first + 5);
            if (colour) {
                blip->rgba  = (uint32_t(ColorByte(args, first + 2, 255)) << 24) | (uint32_t(ColorByte(args, first + 3, 255)) << 16) |
                             (uint32_t(ColorByte(args, first + 4, 255)) << 8) | ColorByte(args, first + 5, 255);
                blip->flags = static_cast<uint8_t>(blip->flags | Blip::kHasColor);
            }
            blip->priority             = static_cast<int16_t>(std::clamp(Num(args, first + 6, 0.0), -32768.0, 32767.0));
            blip->maxDistance          = DistanceArg(Num(args, first + 7, kUnlimitedDistance));
            blip->attachedTo           = attachedTo;
            blip->streaming.alwaysVisible = true; // map-wide, like the story's blips
            id                         = blip.Id();
            if (visibleTo != MafiaNet::UNASSIGNED_PEER_GUID) {
                Framework::Networking::Replication::ViewerFilter filter;
                filter.only = std::unordered_set<MafiaNet::PeerGuid> {visibleTo};
                Replication()->SetViewerFilter(*blip.operator->(), std::move(filter));
            }
        }
        {
            std::lock_guard lock(g_stateMutex);
            if (!resource.empty()) {
                g_elementResource[id] = resource;
            }
            if (visibleTo != MafiaNet::UNASSIGNED_PEER_GUID) {
                g_elementOnlyFor[id] = {visibleTo};
            }
        }
        args.GetReturnValue().Set(gInstance->WrapScriptEntity(isolate, id));
    }

    void CreateBlipCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        if (args.Length() < 3 || !args[0]->IsNumber() || !args[1]->IsNumber() || !args[2]->IsNumber()) {
            Throw(args.GetIsolate(), "createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])");
            return;
        }
        const glm::vec3 pos {static_cast<float>(Num(args, 0, 0)), static_cast<float>(Num(args, 1, 0)), static_cast<float>(Num(args, 2, 0))};
        CreateBlipCommon(args, 3, pos, 0);
    }

    void CreateBlipAttachedToCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t target = ElementId(args, 0);
        glm::vec3 pos {};
        if (!target || !ElementPosition(GetWorld(), target, pos)) {
            Throw(args.GetIsolate(), "createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])");
            return;
        }
        CreateBlipCommon(args, 1, pos, target);
    }

    // Runs `fn` on the blip named by args[0]; throws `usage` if it is not a blip.
    template <typename Fn>
    void WithBlip(const v8::FunctionCallbackInfo<v8::Value> &args, const char *usage, Fn &&fn) {
        const uint64_t id = ElementId(args, 0);
        auto blip         = id ? GetWorld().Write<Blip>(id) : Ref<Blip>();
        if (!blip) {
            Throw(args.GetIsolate(), usage);
            return;
        }
        fn(blip);
    }

    void SetBlipIconCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipIcon(blip, icon)", [&](Ref<Blip> &b) {
            const uint16_t icon = IconArg(args, 1, 0);
            if (icon) {
                b->icon = icon;
            }
            args.GetReturnValue().Set(icon != 0);
        });
    }
    void GetBlipIconCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "getBlipIcon(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<uint32_t>(b->icon)); });
    }
    void SetBlipSizeCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipSize(blip, size)", [&](Ref<Blip> &b) {
            b->scale = static_cast<float>(std::clamp(Num(args, 1, 2.0), 0.0, 50.0) / 2.0);
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipSizeCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "getBlipSize(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<double>(b->scale) * 2.0); });
    }
    void SetBlipColorCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipColor(blip, r, g, b[, a])", [&](Ref<Blip> &b) {
            b->rgba  = (uint32_t(ColorByte(args, 1, 255)) << 24) | (uint32_t(ColorByte(args, 2, 255)) << 16) |
                      (uint32_t(ColorByte(args, 3, 255)) << 8) | ColorByte(args, 4, 255);
            b->flags = static_cast<uint8_t>(b->flags | Blip::kHasColor);
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipColorCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "getBlipColor(blip) -> [r, g, b, a]", [&](Ref<Blip> &b) {
            auto *isolate = args.GetIsolate();
            auto context  = isolate->GetCurrentContext();
            auto result   = v8::Array::New(isolate, 4);
            for (uint32_t i = 0; i < 4; ++i) {
                result->Set(context, i, v8::Integer::New(isolate, static_cast<int>((b->rgba >> (24 - 8 * i)) & 0xff))).Check();
            }
            args.GetReturnValue().Set(result);
        });
    }
    void SetBlipOrderingCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipOrdering(blip, ordering)", [&](Ref<Blip> &b) {
            b->priority = static_cast<int16_t>(std::clamp(Num(args, 1, 0.0), -32768.0, 32767.0));
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipOrderingCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "getBlipOrdering(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<int>(b->priority)); });
    }
    void SetBlipVisibleDistanceCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipVisibleDistance(blip, distance)", [&](Ref<Blip> &b) {
            b->maxDistance = DistanceArg(Num(args, 1, kUnlimitedDistance));
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipVisibleDistanceCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "getBlipVisibleDistance(blip)", [&](Ref<Blip> &b) {
            args.GetReturnValue().Set(b->maxDistance < 0.0f ? static_cast<double>(kUnlimitedDistance) : static_cast<double>(b->maxDistance));
        });
    }
    void SetBlipVisibleCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipVisible(blip, visible)", [&](Ref<Blip> &b) {
            const bool visible = args.Length() > 1 ? args[1]->BooleanValue(args.GetIsolate()) : true;
            b->flags           = static_cast<uint8_t>(visible ? (b->flags & ~Blip::kHidden) : (b->flags | Blip::kHidden));
            args.GetReturnValue().Set(true);
        });
    }
    void SetBlipDisplayCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "setBlipDisplay(blip, \"both\"|\"radar\"|\"map\")", [&](Ref<Blip> &b) {
            const std::string mode = Has(args, 1) ? *v8::String::Utf8Value(args.GetIsolate(), args[1]) : "both";
            uint8_t flags          = static_cast<uint8_t>(b->flags & ~(Blip::kRadarOnly | Blip::kMapOnly));
            if (mode == "radar") {
                flags |= Blip::kRadarOnly;
            }
            else if (mode == "map") {
                flags |= Blip::kMapOnly;
            }
            else if (mode != "both") {
                args.GetReturnValue().Set(false);
                return;
            }
            b->flags = flags;
            args.GetReturnValue().Set(true);
        });
    }

    void DestroyElementCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t id = ElementId(args, 0);
        auto &world       = GetWorld();
        if (!id || !world.Is<Blip>(id)) {
            args.GetReturnValue().Set(false); // players are not destroyable; other elements do not exist yet
            return;
        }
        const bool destroyed = world.Destroy(id);
        if (destroyed) {
            ForgetElement(id);
        }
        args.GetReturnValue().Set(destroyed);
    }

    void AttachElementsCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t target = ElementId(args, 1);
        glm::vec3 pos {};
        if (!target || target == ElementId(args, 0) || !ElementPosition(GetWorld(), target, pos)) {
            Throw(args.GetIsolate(), "attachElements(blip, element)");
            return;
        }
        WithBlip(args, "attachElements(blip, element)", [&](Ref<Blip> &b) {
            b->attachedTo = target;
            b->position   = pos;
            args.GetReturnValue().Set(true);
        });
    }
    void DetachElementsCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        WithBlip(args, "detachElements(blip)", [&](Ref<Blip> &b) {
            const bool was = b->attachedTo != 0;
            b->attachedTo  = 0;
            args.GetReturnValue().Set(was);
        });
    }
    void GetAttachedToCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        uint64_t target = 0;
        bool isBlip     = false;
        WithBlip(args, "getElementAttachedTo(blip)", [&](Ref<Blip> &b) {
            target = b->attachedTo;
            isBlip = true;
        });
        if (!isBlip) {
            return; // WithBlip threw
        }
        args.GetReturnValue().Set(WrapElement(args.GetIsolate(), target && GetWorld().Exists(target) ? target : 0));
    }
    // setElementVisibleTo(blip, player, visible): MTA semantics. A player: let them see it (true) or not (false).
    // null: everyone (true) or nobody (false).
    void SetVisibleToCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate     = args.GetIsolate();
        const char *usage = "setElementVisibleTo(blip, player|null, visible)";
        const uint64_t id = ElementId(args, 0);
        if (!id || !GetWorld().Is<Blip>(id)) {
            Throw(isolate, usage);
            return;
        }
        const bool visible            = args.Length() > 2 ? args[2]->BooleanValue(isolate) : true;
        const uint64_t player         = ElementId(args, 1);
        const MafiaNet::PeerGuid guid = player ? PlayerGuid(player) : MafiaNet::UNASSIGNED_PEER_GUID;
        if (player && guid == MafiaNet::UNASSIGNED_PEER_GUID) {
            Throw(isolate, "setElementVisibleTo: not a connected player");
            return;
        }
        auto blip = GetWorld().Write<Blip>(id);
        if (!blip) {
            Throw(isolate, usage);
            return;
        }
        if (player) {
            Replication()->SetViewerAdmitted(*blip.operator->(), guid, visible);
        }
        else {
            Framework::Networking::Replication::ViewerFilter filter;
            if (!visible) {
                filter.only = std::unordered_set<MafiaNet::PeerGuid> {};
            }
            Replication()->SetViewerFilter(*blip.operator->(), std::move(filter));
            std::lock_guard lock(g_stateMutex);
            g_elementOnlyFor.erase(id);
        }
        args.GetReturnValue().Set(true);
    }

    // Tick thread: attached blips follow their element; a destroyed target leaves the blip where it was
    // (MTA detaches on destroy).
    void UpdateAttachedBlips(World &world) {
        static Clock::time_point next {};
        const auto now = Clock::now();
        if (now < next) {
            return;
        }
        next = now + kAttachInterval;
        std::vector<std::pair<uint64_t, uint64_t>> attached;
        world.ForEachRead<Blip>([&](const auto &blip) {
            if (blip->attachedTo) {
                attached.emplace_back(blip.Id(), blip->attachedTo);
            }
        });
        for (const auto &[blipId, target] : attached) {
            glm::vec3 pos {};
            const bool found = ElementPosition(world, target, pos);
            if (auto blip = world.Write<Blip>(blipId)) {
                if (!found) {
                    blip->attachedTo = 0;
                }
                else if (glm::distance(blip->position, pos) > 0.25f) {
                    blip->position = pos;
                }
            }
        }
    }

    class RDMPServer final: public Framework::Integrations::Server::Instance {
      public:
        void PostInit() override {
            gInstance = this;
            EntityRegistry::Get().Register<Player>(Player::kTypeName);
            EntityRegistry::Get().Register<Blip>(Blip::kTypeName);
            if (const char *v = std::getenv("RDMP_AUTO_RESPAWN")) {
                g_autoRespawn = std::atoi(v) != 0;
            }
            Log()->info("RDMP server {} on Framework {} (game build 0x673783F3){}", GetOptions().modVersion,
                        Framework::Utils::Version::rel, g_autoRespawn ? ", built-in respawn on" : "");
        }

        // Elements belong to the resource that created them and go when it stops (MTA behaviour).
        void PostScriptInit() override {
            auto *module = GetScriptingModule();
            if (!module || !module->GetResourceManager()) {
                return;
            }
            module->GetResourceManager()->AddOnResourceStopped([](const std::string &resource) {
                std::vector<uint64_t> owned;
                {
                    std::lock_guard lock(g_stateMutex);
                    for (const auto &[id, name] : g_elementResource) {
                        if (name == resource) {
                            owned.push_back(id);
                        }
                    }
                }
                auto &world = GetWorld();
                for (const uint64_t id : owned) {
                    world.Destroy(id);
                    ForgetElement(id);
                }
                if (!owned.empty()) {
                    Log()->info("resource {} stopped: destroyed its {} element(s)", resource, owned.size());
                }
            });
        }

        // Deterministic test hook: RDMP_AUTOKILL=<seconds> kills the first player that connects through the
        // same path as setElementHealth(player, 0), so the wasted resource can be verified.
        void MaybeAutoKill(World &world) {
            static bool initialised = false;
            static std::chrono::seconds delay {0};
            static Clock::time_point killAt {};
            static bool done = false;
            if (!initialised) {
                initialised = true;
                if (const char *v = std::getenv("RDMP_AUTOKILL")) {
                    delay = std::chrono::seconds(std::atoi(v));
                }
            }
            if (delay.count() <= 0 || done) {
                return;
            }
            if (killAt == Clock::time_point {}) {
                if (world.IdsOf<Player>().empty()) {
                    return;
                }
                killAt = Clock::now() + delay;
                Log()->info("auto-kill armed: first player dies in {} s", delay.count());
                return;
            }
            if (Clock::now() < killAt) {
                return;
            }
            done = true;
            const auto ids = world.IdsOf<Player>();
            if (ids.empty()) {
                return;
            }
            {
                auto player = world.Write<Player>(ids.front());
                if (!player) {
                    return;
                }
                player->health = 0;
                player->flags  = static_cast<uint8_t>(player->flags & ~Player::kAlive);
                player->ForceState();
                Log()->info("{} wasted (auto-kill test) at ({:.1f}, {:.1f}, {:.1f})", player->nickname, player->position.x,
                            player->position.y, player->position.z);
                std::lock_guard lock(g_stateMutex);
                DeathState &d = g_death[player.Id()];
                d.armed       = false;
                d.dead        = true;
                d.diedAt      = Clock::now();
            }
            EmitPlayerEvent("playerWasted", ids.front());
        }

        void PostUpdate() override {
            auto &world = GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager()->GetWorld();
            FlushActions(world);
            MaybeAutoKill(world);
            UpdateAttachedBlips(world);

            // Wasted: the client reports its real health (localHealth); 0 while alive = death.
            std::vector<uint64_t> wasted, respawn;
            const auto now = Clock::now();
            world.ForEach<Player>([&](const auto &avatar) {
                const uint64_t id = avatar.Id();
                std::lock_guard lock(g_stateMutex);
                DeathState &d = g_death[id];
                if (avatar->localHealth > 0 && !d.dead) {
                    d.armed = true;
                    return;
                }
                if (avatar->localHealth == 0 && d.armed && !d.dead && (avatar->flags & Player::kAlive)) {
                    d.armed        = false;
                    d.dead         = true;
                    d.diedAt       = now;
                    avatar->flags  = static_cast<uint8_t>(avatar->flags & ~Player::kAlive);
                    avatar->health = 0;
                    Log()->info("{} wasted at ({:.1f}, {:.1f}, {:.1f})", avatar->nickname, avatar->position.x,
                                avatar->position.y, avatar->position.z);
                    wasted.push_back(id);
                    return;
                }
                // Optional built-in respawn; normally a gamemode resource does this in its playerWasted handler.
                if (g_autoRespawn && d.dead && now - d.diedAt >= kRespawnDelay) {
                    d.dead         = false;
                    avatar->flags  = static_cast<uint8_t>(avatar->flags | Player::kAlive);
                    avatar->health = 100;
                    respawn.push_back(id);
                    Log()->info("{} respawned (built-in)", avatar->nickname);
                }
            });
            for (const uint64_t id : wasted) {
                EmitPlayerEvent("playerWasted", id);
            }
            for (const uint64_t id : respawn) {
                QueueAction(id, 1 /* Spawn */, kSpawnX, kSpawnY, kSpawnZ, kSpawnHeading);
                EmitPlayerEvent("playerSpawned", id);
            }
        }

        // ---- script events ----
        // Emits a reserved script event ("playerConnected", "playerDisconnected", "playerWasted", "playerSpawned")
        // with the player handle as its only argument. Callable from the packet, tick and script threads.
        void EmitPlayerEvent(const char *name, uint64_t networkId) {
            auto *engine = GetScriptingModule() ? GetScriptingModule()->GetScriptingEngine() : nullptr;
            if (!engine) {
                return;
            }
            PostToScripts([this, engine, eventName = std::string(name), networkId] {
                Framework::Scripting::EngineScope scope(engine);
                if (!scope.Valid()) {
                    return;
                }
                auto player = WrapScriptPlayer(scope.Isolate(), networkId);
                if (player.IsEmpty()) {
                    return;
                }
                std::vector<v8::Local<v8::Value>> args {player};
                GetScriptingModule()->GetResourceManager()->GetEvents().EmitReserved(scope.Isolate(), scope.Context(), eventName, args);
            });
        }

        // Publishes the MTA-style natives as script globals. ModuleRegister runs once per resource context;
        // registering there (inside an EngineScope) is what makes them visible to that resource.
        void ModuleRegister(Framework::Scripting::Engine *engine) override {
            Framework::Scripting::EngineScope scope(engine);
            if (!scope.Valid()) {
                Log()->warn("RDMP script natives: no engine scope");
                return;
            }
            auto *isolate = scope.Isolate();
            auto context  = scope.Context();
            auto global   = context->Global();
            const auto publish = [&](const char *name, v8::FunctionCallback callback) {
                auto fn = v8::Function::New(context, callback).ToLocalChecked();
                global->Set(context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(), fn).Check();
            };
            // players
            publish("spawnPlayer", &SpawnPlayerCallback);
            publish("fadeCamera", &FadeCameraCallback);
            publish("setCameraTarget", &SetCameraTargetCallback);
            publish("setElementControl", &SetControlCallback);
            publish("setElementHealth", &SetHealthCallback);
            publish("getElementHealth", &GetHealthCallback);
            publish("getPlayerName", &GetNameCallback);
            // elements
            publish("getElementPosition", &GetPositionCallback);
            publish("setElementPosition", &SetPositionCallback);
            publish("getElementType", &GetTypeCallback);
            publish("isElement", &IsElementCallback);
            publish("getElementsByType", &GetElementsByTypeCallback);
            publish("destroyElement", &DestroyElementCallback);
            publish("attachElements", &AttachElementsCallback);
            publish("detachElements", &DetachElementsCallback);
            publish("getElementAttachedTo", &GetAttachedToCallback);
            publish("setElementVisibleTo", &SetVisibleToCallback);
            // blips
            publish("createBlip", &CreateBlipCallback);
            publish("createBlipAttachedTo", &CreateBlipAttachedToCallback);
            publish("setBlipIcon", &SetBlipIconCallback);
            publish("getBlipIcon", &GetBlipIconCallback);
            publish("setBlipSize", &SetBlipSizeCallback);
            publish("getBlipSize", &GetBlipSizeCallback);
            publish("setBlipColor", &SetBlipColorCallback);
            publish("getBlipColor", &GetBlipColorCallback);
            publish("setBlipOrdering", &SetBlipOrderingCallback);
            publish("getBlipOrdering", &GetBlipOrderingCallback);
            publish("setBlipVisibleDistance", &SetBlipVisibleDistanceCallback);
            publish("getBlipVisibleDistance", &GetBlipVisibleDistanceCallback);
            publish("setBlipVisible", &SetBlipVisibleCallback);
            publish("setBlipDisplay", &SetBlipDisplayCallback);
            // BlipIcon.BLIP_OBJECTIVE etc.
            auto icons = v8::Object::New(isolate);
            for (const auto &icon : RDMP::Shared::kBlipIcons) {
                icons->Set(context, v8::String::NewFromUtf8(isolate, icon.name).ToLocalChecked(), v8::Integer::New(isolate, icon.id)).Check();
            }
            global->Set(context, v8::String::NewFromUtf8Literal(isolate, "BlipIcon"), icons).Check();
            Log()->info("RDMP script natives registered");
        }

        void OnPlayerConnect(const Framework::Integrations::Server::PlayerConnectionData &data, World &world) override {
            auto *replication = GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager();
            uint64_t id       = 0;
            {
                auto avatar = world.Create<Player>();
                if (!avatar) {
                    Log()->error("avatar allocation failed for {}", data.nickname);
                    return;
                }

                avatar->nickname        = data.nickname;
                avatar->variation       = 12;
                avatar->position        = {kSpawnX, kSpawnY, kSpawnZ};
                avatar->heading         = kSpawnHeading;
                avatar->flags           = Player::kAlive;
                avatar->health          = 100;
                avatar->streaming.range = Player::kStreamRange; // the viewer's reach (framework default: 100 m)

                replication->SetOwner(avatar, data.guid);
                replication->SetViewer(data.guid, avatar);
                id = avatar.Id();
            }

            // Spawn is script-owned: the client parks the player in limbo (rdmp.ini [sandbox] limbo_spawn=1)
            // and the gamemode resource calls spawnPlayer() from its playerConnected handler.
            EmitPlayerEvent("playerConnected", id);
            Log()->info("{} joined (network id {})", data.nickname, id);
        }

        void OnPlayerDisconnect(MafiaNet::PeerGuid guid, World &world) override {
            auto *replication = GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager();
            const uint64_t id = replication->GetViewerId(guid);
            if (const auto avatar = world.Read<Player>(id)) {
                Log()->info("{} left", avatar->nickname);
            }
            // Blips only this player could see (createBlip ... visibleTo) would stay invisible forever: destroy them.
            // Blips attached to the player are detached by UpdateAttachedBlips once the avatar is gone.
            std::vector<uint64_t> orphaned;
            {
                std::lock_guard lock(g_stateMutex);
                for (auto &[blipId, guids] : g_elementOnlyFor) {
                    std::erase(guids, guid);
                    if (guids.empty()) {
                        orphaned.push_back(blipId);
                    }
                }
            }
            for (const uint64_t blipId : orphaned) {
                world.Destroy(blipId);
                ForgetElement(blipId);
            }
            {
                std::lock_guard lock(g_stateMutex);
                g_actions.erase(id);
                g_death.erase(id);
            }
            EmitPlayerEvent("playerDisconnected", id);
        }

        // Server console: players | tp <name> <x> <y> <z> | help (resources: the Framework's own start/stop/restart/refresh)
        void OnConsoleCommand(const std::string &text, const std::string &command, const std::vector<std::string> &args) override {
            (void)text;
            auto &world = GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager()->GetWorld();
            const auto find = [&](const std::string &name) -> uint64_t {
                uint64_t found = 0;
                world.ForEachRead<Player>([&](const auto &p) {
                    if (p->nickname == name) {
                        found = p.Id();
                    }
                });
                return found;
            };
            if (command == "players") {
                world.ForEachRead<Player>([&](const auto &p) {
                    Log()->info("  {} (id {}) at ({:.1f}, {:.1f}, {:.1f}) hp {}", p->nickname, p.Id(), p->position.x, p->position.y,
                                p->position.z, p->localHealth);
                });
                return;
            }
            if (command == "tp" && args.size() >= 4) {
                if (auto p = world.Write<Player>(find(args[0]))) {
                    Teleport(p, std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr), std::strtof(args[3].c_str(), nullptr));
                    Log()->info("teleported {}", args[0]);
                }
                return;
            }
            if (command == "help") {
                Log()->info("RDMP console: players | tp <name> <x> <y> <z>  (Framework: start|stop|restart|refresh <resource>, status)");
            }
        }

        void OnChatCommand(uint64_t senderNetworkId, const std::string &text, const std::string &command,
                           const std::vector<std::string> &args, World &world) override {
            (void)text;
            auto avatar = world.Write<Player>(senderNetworkId);
            if (!avatar) {
                return;
            }

            if (command == "pos") {
                Reply(senderNetworkId, "pos " + std::to_string(avatar->position.x) + " " + std::to_string(avatar->position.y) + " " +
                                           std::to_string(avatar->position.z) + " gait " + std::to_string(avatar->gait) + " hp " +
                                           std::to_string(avatar->localHealth));
                return;
            }
            if (command == "spawn") {
                QueueAction(avatar.Id(), 1 /* Spawn */, kSpawnX, kSpawnY, kSpawnZ, kSpawnHeading);
                Reply(senderNetworkId, "spawned at Armadillo Station");
                return;
            }
            if (command == "fade") {
                const bool fadeIn = args.empty() || std::atoi(args[0].c_str()) != 0;
                const float ms    = args.size() > 1 ? std::strtof(args[1].c_str(), nullptr) : 1000.0f;
                QueueAction(avatar.Id(), 2 /* Fade */, fadeIn ? 1.0f : 0.0f, ms, 0.0f, 0.0f);
                Reply(senderNetworkId, std::string("fadeCamera ") + (fadeIn ? "in" : "out"));
                return;
            }
            if (command == "cam") {
                QueueAction(avatar.Id(), 3 /* CameraTarget */, 0.0f, 0.0f, 0.0f, 0.0f);
                Reply(senderNetworkId, "setCameraTarget(player)");
                return;
            }
            if (command == "tp" && args.size() >= 3) {
                Teleport(avatar, std::strtof(args[0].c_str(), nullptr), std::strtof(args[1].c_str(), nullptr),
                         std::strtof(args[2].c_str(), nullptr));
                Reply(senderNetworkId, "teleported");
                return;
            }
            if (command == "health" && !args.empty()) {
                const float hp = std::clamp(std::strtof(args[0].c_str(), nullptr), 1.0f, 255.0f);
                avatar->health = static_cast<uint8_t>(hp);
                QueueAction(avatar.Id(), 5 /* Health */, hp, 0.0f, 0.0f, 0.0f);
                return;
            }

            Reply(senderNetworkId, "commands: /pos /spawn /tp x y z /health n /fade in|out ms /cam");
        }
    };

    void EmitPlayerEventFwd(const char *name, uint64_t networkId) {
        static_cast<RDMPServer *>(gInstance)->EmitPlayerEvent(name, networkId);
    }
} // namespace

int main(int argc, char **argv) {
    Framework::Integrations::Server::InstanceOptions opts;
    opts.modName       = "RDMP";
    opts.modSlug       = "rdmp";
    opts.modVersion    = "0.4.0";
    opts.gameName      = "Red Dead Redemption";
    opts.gameVersion   = "1.0.42.49788435";
    opts.bindHost      = "127.0.0.1";
    opts.bindPort      = 27015;
    opts.maxPlayers    = 32;
    opts.enableSignals = true;
    opts.argc          = argc;
    opts.argv          = argv;

    RDMPServer server;
    if (const auto result = server.Init(opts); !result) {
        Log()->error("server init failed: {}", result.GetError().message);
        return 1;
    }
    server.Run();
    server.Shutdown();
    return 0;
}
