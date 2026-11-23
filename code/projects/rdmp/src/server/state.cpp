/*
 * Server state shared by the API modules and the instance: see server.h.
 */

#include "server/server.h"

#include <networking/rpc/chat_message.h>
#include <scripting/engine_scope.h>
#include <scripting/resource/resource.h>
#include <scripting/resource/resource_manager.h>
#include <v8pp/convert.hpp>

#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace RDMP::Server {
    namespace Builtins = Framework::Scripting::Builtins;

    namespace {
        Framework::Integrations::Server::Instance *g_instance = nullptr;
        std::mutex g_stateMutex;

        constexpr auto kActionResend    = std::chrono::milliseconds(300); // un-acked action: push the forced state again
        constexpr int kActionMaxResends = 10;                             // then drop it (client gone or wedged)

        struct PendingAction {
            ActionKind kind;
            float a, b, c, d;
            std::string text;
        };
        // One slot on the entity, so the next action goes only after the client echoed actAck == actSeq.
        struct ActionChannel {
            std::deque<PendingAction> queue;
            uint8_t inFlight = 0; // seq waiting for its ack, 0 = none
            Clock::time_point sentAt {};
            int resends = 0;
        };
        std::map<uint64_t, ActionChannel> g_actions;
        uint8_t g_actionSeq = 0;

        // `armed` once the player has been seen alive since the last death, so the stale 0 that lingers until the
        // respawn cannot kill twice.
        struct DeathState {
            bool armed = false;
            bool dead  = false;
            Clock::time_point diedAt {};
        };
        std::map<uint64_t, DeathState> g_death;
        constexpr auto kRespawnDelay = std::chrono::seconds(5); // built-in respawn (RDMP_AUTO_RESPAWN=1)

        std::unordered_map<uint64_t, std::string> g_elementResource;
        std::unordered_map<uint64_t, std::vector<MafiaNet::PeerGuid>> g_elementOnlyFor;

        uint8_t NextActionSeq() {
            if (++g_actionSeq == 0) {
                g_actionSeq = 1; // 0 means "never sent"
            }
            return g_actionSeq;
        }
    } // namespace

    // ---- instance ----
    Framework::Integrations::Server::Instance *Instance() {
        return g_instance;
    }
    void SetInstance(Framework::Integrations::Server::Instance *instance) {
        g_instance = instance;
    }
    std::shared_ptr<spdlog::logger> Log() {
        return Framework::Logging::GetLogger(FRAMEWORK_INNER_SERVER);
    }
    World &GetWorld() {
        return g_instance->GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager()->GetWorld();
    }
    Framework::Networking::Replication::ReplicationManager *Replication() {
        return g_instance->GetNetworkingEngine()->GetNetworkServer()->GetReplicationManager();
    }
    std::string CallingResource(v8::Isolate *isolate) {
        auto *module   = g_instance->GetScriptingModule();
        auto *manager  = module ? module->GetResourceManager() : nullptr;
        auto *resource = manager ? manager->GetCurrentResourceWithStackFallback(isolate) : nullptr;
        return resource ? resource->GetName() : std::string();
    }

    // ---- argument helpers ----
    uint64_t ElementId(const Args &args, int i) {
        if (!ScriptApi::Has(args, i) || !args[i]->IsObject()) {
            return 0;
        }
        auto *handle = v8pp::from_v8<Builtins::Entity *>(args.GetIsolate(), args[i]);
        return handle ? handle->GetId() : 0;
    }
    MafiaNet::PeerGuid PlayerGuid(uint64_t playerId) {
        MafiaNet::PeerGuid guid = MafiaNet::UNASSIGNED_PEER_GUID;
        if (playerId && GetWorld().Is<Player>(playerId)) {
            Builtins::Entity(playerId).ResolveOwner(guid);
        }
        return guid;
    }
    Ref<Player> PlayerFromArg(const Args &args, int index) {
        const uint64_t id = ElementId(args, index);
        return id ? GetWorld().Write<Player>(id) : Ref<Player>();
    }
    v8::Local<v8::Value> WrapElement(v8::Isolate *isolate, uint64_t id) {
        if (!id) {
            return v8::Null(isolate);
        }
        if (GetWorld().Is<Player>(id)) {
            return g_instance->WrapScriptPlayer(isolate, id);
        }
        return g_instance->WrapScriptEntity(isolate, id);
    }
    bool ElementPosition(World &world, uint64_t id, glm::vec3 &out) {
        if (auto p = world.Read<Player>(id)) {
            out = p->position;
            return true;
        }
        if (auto b = world.Read<Blip>(id)) {
            out = b->position;
            return true;
        }
        if (auto v = world.Read<RDMP::Shared::Vehicle>(id)) {
            out = v->position;
            return true;
        }
        return false;
    }

    // ---- state ----
    std::mutex &StateMutex() {
        return g_stateMutex;
    }

    void OwnElement(uint64_t id, const std::string &resource, MafiaNet::PeerGuid onlyFor) {
        std::lock_guard lock(g_stateMutex);
        if (!resource.empty()) {
            g_elementResource[id] = resource;
        }
        if (onlyFor != MafiaNet::UNASSIGNED_PEER_GUID) {
            g_elementOnlyFor[id] = {onlyFor};
        }
    }
    void ForgetElement(uint64_t id) {
        std::lock_guard lock(g_stateMutex);
        g_elementResource.erase(id);
        g_elementOnlyFor.erase(id);
    }
    void ForgetVisibleOnly(uint64_t id) {
        std::lock_guard lock(g_stateMutex);
        g_elementOnlyFor.erase(id);
    }
    void DestroyResourceElements(const std::string &resource) {
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
    }
    void DestroyElementsOnlyFor(MafiaNet::PeerGuid guid) {
        std::vector<uint64_t> orphaned;
        {
            std::lock_guard lock(g_stateMutex);
            for (auto &[id, guids] : g_elementOnlyFor) {
                std::erase(guids, guid);
                if (guids.empty()) {
                    orphaned.push_back(id);
                }
            }
        }
        auto &world = GetWorld();
        for (const uint64_t id : orphaned) {
            world.Destroy(id);
            ForgetElement(id);
        }
    }

    void QueueAction(uint64_t avatarId, ActionKind kind, float a, float b, float c, float d, const std::string &text) {
        std::lock_guard lock(g_stateMutex);
        g_actions[avatarId].queue.push_back({kind, a, b, c, d, text});
    }

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
            PendingAction action = std::move(ch.queue.front());
            ch.queue.pop_front();
            avatar->actSeq  = NextActionSeq();
            avatar->actKind = static_cast<uint8_t>(action.kind);
            avatar->actA    = action.a;
            avatar->actB    = action.b;
            avatar->actC    = action.c;
            avatar->actD    = action.d;
            avatar->actText = std::move(action.text);
            avatar->ForceState();
            ch.inFlight = avatar->actSeq;
            ch.sentAt   = now;
            ch.resends  = 0;
        }
    }

    void ForgetPlayer(uint64_t avatarId) {
        std::lock_guard lock(g_stateMutex);
        g_actions.erase(avatarId);
        g_death.erase(avatarId);
    }

    void MarkDead(uint64_t id) {
        std::lock_guard lock(g_stateMutex);
        DeathState &d = g_death[id];
        d.armed       = false;
        d.dead        = true;
        d.diedAt      = Clock::now();
    }

    void MarkRespawned(uint64_t id) {
        std::lock_guard lock(g_stateMutex);
        g_death[id].dead = false;
    }

    void UpdateDeaths(World &world, bool autoRespawn, std::vector<uint64_t> &wasted, std::vector<uint64_t> &respawned) {
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
                Log()->info("{} wasted at ({:.1f}, {:.1f}, {:.1f})", avatar->nickname, avatar->position.x, avatar->position.y, avatar->position.z);
                wasted.push_back(id);
                return;
            }
            if (autoRespawn && d.dead && now - d.diedAt >= kRespawnDelay) {
                d.dead         = false;
                avatar->flags  = static_cast<uint8_t>(avatar->flags | Player::kAlive);
                avatar->health = 100;
                respawned.push_back(id);
                Log()->info("{} respawned (built-in)", avatar->nickname);
            }
        });
    }

    void Teleport(const Ref<Player> &avatar, float x, float y, float z) {
        avatar->position = {x, y, z};
        avatar->speed    = 0.0f;
        avatar->ForceState();
    }

    // ---- chat ----
    void SendChat(MafiaNet::PeerGuid to, const std::string &text, const std::string &author, uint32_t colorRGBA, uint64_t senderNetworkId) {
        auto *server = g_instance ? g_instance->GetNetworkingEngine()->GetNetworkServer() : nullptr;
        if (!server) {
            return;
        }
        Framework::Networking::RPC::ChatMessage message;
        message.text            = text;
        message.author          = author;
        message.color           = colorRGBA;
        message.senderNetworkId = senderNetworkId;
        if (to == MafiaNet::UNASSIGNED_PEER_GUID) {
            server->BroadcastRPC(message);
        }
        else {
            server->SendRPC(message, MafiaNet::RakNetGUID(static_cast<uint64_t>(to)));
        }
    }

    // ---- events ----
    void EmitPlayerEventArgs(const char *name, uint64_t networkId, std::vector<EventArg> extra) {
        auto *module = g_instance ? g_instance->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine) {
            return;
        }
        g_instance->PostToScripts([engine, eventName = std::string(name), networkId, extra = std::move(extra)] {
            Framework::Scripting::EngineScope scope(engine);
            if (!scope.Valid()) {
                return;
            }
            auto *isolate = scope.Isolate();
            auto player   = g_instance->WrapScriptPlayer(isolate, networkId);
            if (player.IsEmpty()) {
                return;
            }
            std::vector<v8::Local<v8::Value>> args {player};
            for (const auto &a : extra) {
                switch (a.kind) {
                case EventArg::Kind::Text: args.push_back(ScriptApi::JsString(isolate, a.text)); break;
                case EventArg::Kind::Number: args.push_back(v8::Number::New(isolate, a.number)); break;
                case EventArg::Kind::Element: args.push_back(a.element ? WrapElement(isolate, a.element) : v8::Null(isolate).As<v8::Value>()); break;
                }
            }
            g_instance->GetScriptingModule()->GetResourceManager()->GetEvents().EmitReserved(isolate, scope.Context(), eventName, args);
        });
    }

    void EmitPlayerEvent(const char *name, uint64_t networkId, std::vector<std::string> extra) {
        auto *module = g_instance ? g_instance->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine) {
            return;
        }
        g_instance->PostToScripts([engine, eventName = std::string(name), networkId, extra = std::move(extra)] {
            Framework::Scripting::EngineScope scope(engine);
            if (!scope.Valid()) {
                return;
            }
            auto player = g_instance->WrapScriptPlayer(scope.Isolate(), networkId);
            if (player.IsEmpty()) {
                return;
            }
            std::vector<v8::Local<v8::Value>> args {player};
            for (const auto &s : extra) {
                args.push_back(ScriptApi::JsString(scope.Isolate(), s));
            }
            g_instance->GetScriptingModule()->GetResourceManager()->GetEvents().EmitReserved(scope.Isolate(), scope.Context(), eventName, args);
        });
    }
} // namespace RDMP::Server
