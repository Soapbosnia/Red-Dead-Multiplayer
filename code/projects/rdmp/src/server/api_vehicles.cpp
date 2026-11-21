/*
 * SERVER vehicles (MTA names; a vehicle is a horse for now, shared/vehicle.h):
 *   createVehicle(model, x, y, z[, rz]) -> vehicle
 *   getVehicleModel(vehicle) -> model
 *   warpPedIntoVehicle(player, vehicle) -> bool
 *   removePedFromVehicle(player) -> bool
 *   getPedOccupiedVehicle(player) -> vehicle | false
 *   getVehicleOccupant(vehicle) -> player | false
 * Events: playerVehicleEnter(player, vehicle), playerVehicleExit(player, vehicle).
 *
 * Who rides what: the server decides (Player::vehicle, Vehicle::occupant). A script warps a player in or out; a player
 * who mounts or leaves a horse with the game's own controls reports it (Player::localVehicle) and the server accepts
 * it when the horse is free. The rider's client owns the vehicle while riding (it syncs the horse, MTA's driver
 * sync); otherwise the server does and every client shows it standing.
 */

#include "server/server.h"

#include <scripting/engine_scope.h>
#include <scripting/resource/resource_manager.h>

#include <map>
#include <mutex>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;
        using RDMP::Shared::Vehicle;

        constexpr size_t kMaxVehicles = 512;
        // After the server put a player on (or off) a vehicle, the client's own report lags a round trip plus the mount
        // animation: during this window a report that disagrees is the old state, not a change of mind.
        constexpr auto kAssignGrace = std::chrono::milliseconds(3000);

        std::mutex g_assignMutex; // script thread (warps) and tick thread (reports)
        std::map<uint64_t, Clock::time_point> g_assignedAt; // player -> when the server last changed their vehicle

        void MarkAssigned(uint64_t player) {
            std::lock_guard lock(g_assignMutex);
            g_assignedAt[player] = Clock::now();
        }
        bool AssignedRecently(uint64_t player, Clock::time_point now) {
            std::lock_guard lock(g_assignMutex);
            const auto it = g_assignedAt.find(player);
            return it != g_assignedAt.end() && now - it->second < kAssignGrace;
        }
        void ForgetAssigned(uint64_t player) {
            std::lock_guard lock(g_assignMutex);
            g_assignedAt.erase(player);
        }

        void EmitVehicleEvent(const char *name, uint64_t playerId, uint64_t vehicleId) {
            auto *module = Instance() ? Instance()->GetScriptingModule() : nullptr;
            auto *engine = module ? module->GetScriptingEngine() : nullptr;
            if (!engine) {
                return;
            }
            Instance()->PostToScripts([engine, eventName = std::string(name), playerId, vehicleId] {
                Framework::Scripting::EngineScope scope(engine);
                if (!scope.Valid()) {
                    return;
                }
                auto player = Instance()->WrapScriptPlayer(scope.Isolate(), playerId);
                if (player.IsEmpty()) {
                    return;
                }
                std::vector<v8::Local<v8::Value>> args {player, WrapElement(scope.Isolate(), vehicleId)};
                Instance()->GetScriptingModule()->GetResourceManager()->GetEvents().EmitReserved(scope.Isolate(), scope.Context(), eventName,
                                                                                                args);
            });
        }

        // Puts the player on the vehicle (both locked by the caller, who looked up the rider's connection before: no
        // world lookups while handles are held). The rider's client becomes the vehicle's owner.
        void Seat(Ref<Player> &player, Ref<Vehicle> &vehicle, MafiaNet::PeerGuid riderGuid) {
            player->vehicle   = vehicle.Id();
            vehicle->occupant = player.Id();
            Replication()->SetOwner(vehicle, riderGuid);
            player->ForceState(); // the owner hears its server-side vehicle through a forced state
        }

        // Takes the player off the vehicle (both locked by the caller); the server owns it again.
        void Unseat(Ref<Player> &player, Ref<Vehicle> &vehicle) {
            if (player) {
                player->vehicle = 0;
                player->ForceState();
            }
            if (vehicle) {
                vehicle->occupant = 0;
                vehicle->speed    = 0.0f;
                vehicle->moveRight = vehicle->moveBack = 0.0f;
                vehicle->gait      = 0;
                Replication()->SetOwner(vehicle, MafiaNet::UNASSIGNED_PEER_GUID);
            }
        }

        // Leaves the player's current vehicle (if any), firing playerVehicleExit. No handles may be held.
        void ExitVehicle(uint64_t playerId) {
            auto &world        = GetWorld();
            uint64_t vehicleId = 0;
            if (auto p = world.Read<Player>(playerId)) {
                vehicleId = p->vehicle;
            }
            if (!vehicleId) {
                return;
            }
            {
                auto [p, v] = world.Write<Player, Vehicle>(playerId, vehicleId);
                Unseat(p, v);
            }
            MarkAssigned(playerId);
            EmitVehicleEvent("playerVehicleExit", playerId, vehicleId);
        }

        // Seats the player (leaving another vehicle first). False when the vehicle is taken or gone.
        bool EnterVehicle(uint64_t playerId, uint64_t vehicleId) {
            auto &world = GetWorld();
            {
                auto v = world.Read<Vehicle>(vehicleId);
                if (!v || (v->occupant && v->occupant != playerId)) {
                    return false;
                }
            }
            uint64_t current = 0;
            if (auto p = world.Read<Player>(playerId)) {
                current = p->vehicle;
            }
            if (current == vehicleId) {
                return true;
            }
            if (current) {
                ExitVehicle(playerId);
            }
            const MafiaNet::PeerGuid guid = PlayerGuid(playerId);
            {
                auto [p, v] = world.Write<Player, Vehicle>(playerId, vehicleId);
                if (!p || !v || (v->occupant && v->occupant != playerId)) {
                    return false;
                }
                Seat(p, v, guid);
            }
            MarkAssigned(playerId);
            EmitVehicleEvent("playerVehicleEnter", playerId, vehicleId);
            return true;
        }

        template <typename Fn>
        void WithVehicle(const Args &args, int index, const char *usage, Fn &&fn) {
            const uint64_t id = ElementId(args, index);
            if (!id || !GetWorld().Is<Vehicle>(id)) {
                Throw(args.GetIsolate(), usage);
                return;
            }
            fn(id);
        }

        // createVehicle(model, x, y, z[, rz]) -> vehicle. model: actor definition 976..1005 (RIDEABLE_ANIMAL_*).
        void CreateVehicleCallback(const Args &args) {
            auto *isolate = args.GetIsolate();
            if (args.Length() < 4 || !args[0]->IsNumber() || !args[1]->IsNumber() || !args[2]->IsNumber() || !args[3]->IsNumber()) {
                Throw(isolate, "createVehicle(model, x, y, z[, rz])");
                return;
            }
            const double model = Num(args, 0, Vehicle::kDefaultModel);
            if (model < Vehicle::kModelMin || model > Vehicle::kModelMax) {
                Throw(isolate, "createVehicle: model must be a rideable animal, 976..1005");
                return;
            }
            auto &world = GetWorld();
            if (world.IdsOf<Vehicle>().size() >= kMaxVehicles) {
                Log()->warn("createVehicle: the server already has {} vehicles", kMaxVehicles);
                args.GetReturnValue().Set(false);
                return;
            }
            uint64_t id = 0;
            {
                auto v = world.Create<Vehicle>();
                if (!v) {
                    args.GetReturnValue().Set(false);
                    return;
                }
                v->model           = static_cast<uint32_t>(model);
                v->position        = {static_cast<float>(Num(args, 1, 0)), static_cast<float>(Num(args, 2, 0)), static_cast<float>(Num(args, 3, 0))};
                v->heading         = static_cast<float>(Num(args, 4, 0.0));
                v->streaming.range = Vehicle::kStreamRange;
                id                 = v.Id();
            }
            OwnElement(id, CallingResource(isolate), MafiaNet::UNASSIGNED_PEER_GUID);
            args.GetReturnValue().Set(Instance()->WrapScriptEntity(isolate, id));
        }

        void GetVehicleModelCallback(const Args &args) {
            WithVehicle(args, 0, "getVehicleModel(vehicle)", [&](uint64_t id) {
                if (auto v = GetWorld().Read<Vehicle>(id)) {
                    args.GetReturnValue().Set(v->model);
                }
            });
        }

        void WarpPedIntoVehicleCallback(const Args &args) {
            const uint64_t player = ElementId(args, 0);
            if (!player || !GetWorld().Is<Player>(player)) {
                Throw(args.GetIsolate(), "warpPedIntoVehicle(player, vehicle)");
                return;
            }
            WithVehicle(args, 1, "warpPedIntoVehicle(player, vehicle)", [&](uint64_t id) { args.GetReturnValue().Set(EnterVehicle(player, id)); });
        }

        void RemovePedFromVehicleCallback(const Args &args) {
            const uint64_t player = ElementId(args, 0);
            if (!player || !GetWorld().Is<Player>(player)) {
                Throw(args.GetIsolate(), "removePedFromVehicle(player)");
                return;
            }
            bool riding = false;
            if (auto p = GetWorld().Read<Player>(player)) {
                riding = p->vehicle != 0;
            }
            ExitVehicle(player);
            args.GetReturnValue().Set(riding);
        }

        void GetPedOccupiedVehicleCallback(const Args &args) {
            const uint64_t player = ElementId(args, 0);
            uint64_t vehicle      = 0;
            if (auto p = player ? GetWorld().Read<Player>(player) : ConstRef<Player>()) {
                vehicle = p->vehicle;
            }
            if (vehicle && GetWorld().Exists(vehicle)) {
                args.GetReturnValue().Set(WrapElement(args.GetIsolate(), vehicle));
            }
            else {
                args.GetReturnValue().Set(false);
            }
        }

        void GetVehicleOccupantCallback(const Args &args) {
            WithVehicle(args, 0, "getVehicleOccupant(vehicle)", [&](uint64_t id) {
                uint64_t occupant = 0;
                if (auto v = GetWorld().Read<Vehicle>(id)) {
                    occupant = v->occupant;
                }
                if (occupant && GetWorld().Exists(occupant)) {
                    args.GetReturnValue().Set(Instance()->WrapScriptPlayer(args.GetIsolate(), occupant));
                }
                else {
                    args.GetReturnValue().Set(false);
                }
            });
        }
    } // namespace

    void RegisterVehicleApi(const ScriptApi::Publisher &publish) {
        publish("createVehicle", &CreateVehicleCallback);
        publish("getVehicleModel", &GetVehicleModelCallback);
        publish("warpPedIntoVehicle", &WarpPedIntoVehicleCallback);
        publish("removePedFromVehicle", &RemovePedFromVehicleCallback);
        publish("getPedOccupiedVehicle", &GetPedOccupiedVehicleCallback);
        publish("getVehicleOccupant", &GetVehicleOccupantCallback);
    }

    // Tick thread: what the clients report against what the server decided.
    //  - a client on a vehicle the server has not given it (mounted with the game's own controls): accepted when the
    //    vehicle is free, else the server's state stands (the client dismounts when its state is not confirmed);
    //  - a client off the vehicle the server gave it, after the grace: it dismounted itself.
    void UpdateVehicles(World &world) {
        const auto now = Clock::now();
        struct Report {
            uint64_t player, assigned, local;
        };
        std::vector<Report> reports;
        world.ForEachRead<Player>([&](const ConstRef<Player> &p) { reports.push_back({p.Id(), p->vehicle, p->localVehicle}); });
        for (const Report &r : reports) {
            if (r.assigned && !world.Is<Vehicle>(r.assigned)) { // destroyed under its rider: on foot again
                if (auto p = world.Write<Player>(r.player)) {
                    p->vehicle = 0;
                    p->ForceState();
                }
                MarkAssigned(r.player);
                EmitVehicleEvent("playerVehicleExit", r.player, r.assigned);
                continue;
            }
            if (r.local == r.assigned) {
                continue;
            }
            if (AssignedRecently(r.player, now)) {
                continue;
            }
            if (r.local && world.Is<Vehicle>(r.local)) {
                if (!EnterVehicle(r.player, r.local)) {
                    MarkAssigned(r.player); // taken: tell the client again after the grace
                    if (auto p = world.Write<Player>(r.player)) {
                        p->ForceState();
                    }
                }
            }
            else if (!r.local && r.assigned) {
                ExitVehicle(r.player);
            }
        }
        // a vehicle whose rider is gone is free again
        std::vector<std::pair<uint64_t, uint64_t>> ridden; // (vehicle, rider): collected first, no lookups under a handle
        world.ForEachRead<Vehicle>([&](const ConstRef<Vehicle> &v) {
            if (v->occupant) {
                ridden.emplace_back(v.Id(), v->occupant);
            }
        });
        for (const auto &[vehicleId, playerId] : ridden) {
            if (world.Is<Player>(playerId)) {
                continue;
            }
            if (auto v = world.Write<Vehicle>(vehicleId)) {
                Ref<Player> none;
                Unseat(none, v);
            }
            ForgetAssigned(playerId);
        }
    }

    // A player leaves: their vehicle is free again (before their avatar goes).
    void LeaveVehicleOnDisconnect(uint64_t playerId) {
        uint64_t vehicleId = 0;
        if (auto p = GetWorld().Read<Player>(playerId)) {
            vehicleId = p->vehicle;
        }
        if (vehicleId) {
            ExitVehicle(playerId);
        }
        ForgetAssigned(playerId);
    }
} // namespace RDMP::Server
