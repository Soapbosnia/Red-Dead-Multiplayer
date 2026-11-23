/*
 * RDMP (Red Dead Multiplayer) dedicated server, on the MafiaHub Framework 44.
 *
 * One avatar entity per connection, owned by that connection and used as its viewer, so a player's position decides
 * what the server streams to them. The script API (MTA-style) lives in api_*.cpp and is published into every resource
 * context from ModuleRegister; server.h describes how the files fit together.
 */

#include "server/server.h"

#include <networking/replication/entity_registry.h>
#include <scripting/engine_scope.h>
#include <scripting/resource/resource_manager.h>
#include <http/webserver.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <utils/version.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <vector>

namespace {
    using namespace RDMP::Server;
    using Framework::Networking::Replication::EntityRegistry;

    class RDMPServer final: public Framework::Integrations::Server::Instance {
      public:
        void PostInit() override {
            SetInstance(this);
            EntityRegistry::Get().Register<Player>(Player::kTypeName);
            EntityRegistry::Get().Register<Blip>(Blip::kTypeName);
            EntityRegistry::Get().Register<RDMP::Shared::Vehicle>(RDMP::Shared::Vehicle::kTypeName);
            if (const char *v = std::getenv("RDMP_AUTO_RESPAWN")) {
                _autoRespawn = std::atoi(v) != 0;
            }
            LoadServerName();
            if (auto *web = GetWebserver()) {
                web->RegisterRequest("/rdmp", [this](const httplib::Request &, httplib::Response &res) {
                    ServeInfo(res);
                });
            }
            Log()->info("RDMP server \"{}\" {} on Framework {} (game build 0x673783F3){}", _name, GetOptions().modVersion,
                        Framework::Utils::Version::rel, _autoRespawn ? ", built-in respawn on" : "");
        }

        // server.json "name" (the Framework ignores keys it does not know): what server browsers show.
        void LoadServerName() {
            std::ifstream file(GetOptions().modConfigFile);
            if (!file) {
                return;
            }
            const auto doc = nlohmann::json::parse(file, nullptr, false);
            if (doc.is_object() && doc.contains("name") && doc["name"].is_string() && !doc["name"].get<std::string>().empty()) {
                _name = doc["name"].get<std::string>();
            }
        }

        // GET /rdmp on the game port: what a server browser lists (the Framework's GET / has no name or players).
        void ServeInfo(httplib::Response &res) {
            nlohmann::json info;
            info["name"]       = _name;
            info["version"]    = GetOptions().modVersion;
            info["players"]    = _playerCount.load(); // this runs on the web server's thread: no world access here
            info["maxPlayers"] = GetOptions().maxPlayers;
            info["password"]   = !GetOptions().bindPassword.empty();
            res.set_content(info.dump(), "application/json");
            res.status = 200;
        }

        // Elements belong to the resource that created them and go when it stops (MTA behaviour).
        void PostScriptInit() override {
            auto *module = GetScriptingModule();
            if (module && module->GetResourceManager()) {
                module->GetResourceManager()->AddOnResourceStopped([](const std::string &resource) {
                    DestroyResourceElements(resource);
                });
            }
        }

        void PostUpdate() override {
            auto &world = GetWorld();
            _playerCount = static_cast<int>(world.IdsOf<Player>().size());
            FlushActions(world);
            UpdateAttachedBlips(world);
            UpdateVehicles(world);
            UpdateWeapons(world);

            // Wasted: the client reports its real health (localHealth); 0 while alive = death.
            std::vector<uint64_t> wasted, respawned;
            UpdateDeaths(world, _autoRespawn, wasted, respawned);
            for (const uint64_t id : wasted) {
                EmitPlayerEvent("playerWasted", id);
            }
            for (const uint64_t id : respawned) { // RDMP_AUTO_RESPAWN=1 only; normally a resource respawns
                QueueAction(id, ActionKind::Spawn, kSpawnX, kSpawnY, kSpawnZ, kSpawnHeading);
                EmitPlayerEvent("playerSpawned", id);
            }
        }

        // ModuleRegister runs once per resource context; registering there (inside an EngineScope) is what makes the
        // functions visible to that resource.
        void ModuleRegister(Framework::Scripting::Engine *engine) override {
            Framework::Scripting::EngineScope scope(engine);
            if (!scope.Valid()) {
                Log()->warn("RDMP script API: no engine scope");
                return;
            }
            const auto publish = RDMP::ScriptApi::GlobalPublisher(scope.Isolate(), scope.Context());
            RDMP::ScriptApi::RegisterShared(scope.Isolate(), scope.Context(), publish);
            RegisterPlayerApi(publish);
            RegisterElementApi(publish);
            RegisterBlipApi(publish);
            RegisterHudApi(publish);
            RegisterVehicleApi(publish);
            RegisterWeaponApi(publish);
            Log()->info("RDMP script API registered");
        }

        void OnPlayerConnect(const Framework::Integrations::Server::PlayerConnectionData &data, World &world) override {
            uint64_t id = 0;
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
                Replication()->SetOwner(avatar, data.guid);
                Replication()->SetViewer(data.guid, avatar);
                id = avatar.Id();
            }
            // Spawn is script-owned: the client parks the player in limbo until a resource calls spawnPlayer().
            EmitPlayerEvent("playerConnected", id);
            Log()->info("{} joined (network id {})", data.nickname, id);
        }

        void OnPlayerDisconnect(MafiaNet::PeerGuid guid, World &world) override {
            const uint64_t id = Replication()->GetViewerId(guid);
            if (const auto avatar = world.Read<Player>(id)) {
                Log()->info("{} left", avatar->nickname);
            }
            // Blips only this player could see would stay invisible forever; attached ones detach on their own.
            DestroyElementsOnlyFor(guid);
            LeaveVehicleOnDisconnect(id);
            ForgetPlayer(id);
            ForgetPlayerWeapons(id);
            EmitPlayerEvent("playerDisconnected", id);
        }

        // Plain chat: the playerChat event first (a handler returning false keeps the line), then everyone sees it.
        void OnChatMessage(uint64_t senderNetworkId, const std::string &text, World &world) override {
            std::string name;
            if (const auto avatar = world.Read<Player>(senderNetworkId)) {
                name = avatar->nickname;
            }
            if (name.empty()) {
                return;
            }
            bool allowed = true;
            RunOnScriptThread([&] {
                auto *module = GetScriptingModule();
                auto *engine = module ? module->GetScriptingEngine() : nullptr;
                if (!engine) {
                    return;
                }
                Framework::Scripting::EngineScope scope(engine);
                if (!scope.Valid()) {
                    return;
                }
                auto player = WrapScriptPlayer(scope.Isolate(), senderNetworkId);
                if (player.IsEmpty()) {
                    return;
                }
                std::vector<v8::Local<v8::Value>> args {player, RDMP::ScriptApi::JsString(scope.Isolate(), text)};
                allowed = module->GetResourceManager()->GetEvents().EmitReservedSync(scope.Isolate(), scope.Context(), "playerChat", args);
            });
            if (allowed) {
                Log()->info("[chat] {}: {}", name, text);
                SendChat(MafiaNet::UNASSIGNED_PEER_GUID, text, name, 0, senderNetworkId);
            }
        }

        // "/command args": the playerCommand event (player, command, ...args).
        void OnChatCommand(uint64_t senderNetworkId, const std::string &text, const std::string &command, const std::vector<std::string> &args,
                           World &world) override {
            (void)text;
            (void)world;
            EmitPlayerEvent("playerCommand", senderNetworkId, [&] {
                std::vector<std::string> extra {command};
                extra.insert(extra.end(), args.begin(), args.end());
                return extra;
            }());
        }

        // Server console: players | tp <name> <x> <y> <z> | help (resources: the Framework's start/stop/restart/refresh).
        void OnConsoleCommand(const std::string &text, const std::string &command, const std::vector<std::string> &args) override {
            (void)text;
            auto &world = GetWorld();
            if (command == "players") {
                world.ForEachRead<Player>([&](const auto &p) {
                    Log()->info("  {} (id {}) at ({:.1f}, {:.1f}, {:.1f}) hp {}", p->nickname, p.Id(), p->position.x, p->position.y, p->position.z,
                                p->localHealth);
                });
                return;
            }
            if (command == "tp" && args.size() >= 4) {
                uint64_t found = 0;
                world.ForEachRead<Player>([&](const auto &p) {
                    if (p->nickname == args[0]) {
                        found = p.Id();
                    }
                });
                if (auto p = world.Write<Player>(found)) {
                    Teleport(p, std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr), std::strtof(args[3].c_str(), nullptr));
                    Log()->info("teleported {}", args[0]);
                }
                return;
            }
            if (command == "help") {
                Log()->info("RDMP console: players | tp <name> <x> <y> <z>  (Framework: start|stop|restart|refresh <resource>, status)");
            }
        }

      private:
        bool _autoRespawn = false; // RDMP_AUTO_RESPAWN=1: respawn at Armadillo 5 s after death (no resource needed)
        std::string _name  = "Red Dead Multiplayer server";
        std::atomic<int> _playerCount {0};
    };
} // namespace

int main(int argc, char **argv) {
    Framework::Integrations::Server::InstanceOptions opts;
    opts.modName       = "RDMP";
    opts.modSlug       = "rdmp";
    opts.modVersion    = "0.5.0";
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
