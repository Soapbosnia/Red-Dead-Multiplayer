// The server's vehicles in the game: see vehicles.h.
#include "vehicles.h"

#include <windows.h>

#include <cmath>
#include <map>

#include "locomotion.h"
#include "log.h"
#include "puppet_control.h"
#include "remote_timeline.h"
#include "sandbox.h"
#include "scriptnatives.h"

namespace rdmp {
namespace {

struct Horse {
    uint32_t goh = 0;
    bool hidden = false;
    uint32_t model = 0;
    Remote timeline;      // network states (when someone else syncs it)
    PuppetDrive drive;    // the puppet controller's state for it
    ULONGLONG lastSeen = 0;
};

std::map<uint64_t, Horse> g_horses;
float g_streamIn = 150.0f, g_streamOut = 200.0f;

// The local player's mount, as the game has it, and the server's word on it.
uint64_t g_localVehicle = 0;
uint64_t g_lastAssigned = 0;
uint64_t g_pendingMount = 0;          // assigned but its horse was not here yet
double g_mismatchSince = 0;           // mounted on a vehicle the server has not confirmed: since (0 = not)
uint32_t g_ridingActorWatch = 0;      // horse goh whose desire we capture (the one we ride)
Vec3 g_lastOwnPos{};
double g_lastOwnMs = 0;
float g_ownSpeed = 0.0f;

uint32_t CallU(const char* name, std::initializer_list<uint32_t> args) {
    NativeCall c;
    for (uint32_t a : args) c.Uint(a);
    return c.Call(NativeHash(name)) ? c.ResultUint() : 0;
}

uint64_t VehicleOfGoh(uint32_t goh) {
    if (!goh) return 0;
    for (const auto& [id, h] : g_horses)
        if (h.goh == goh && !h.hidden) return id;
    return 0;
}

// The local player's mount against the server: follow the server's changes (warps), report the game's (own controls),
// give up a mount the server does not confirm.
void LocalMountFrame(const fw::Api& api, double nowMs) {
    const uint32_t player = SandboxPlayerGoh();
    const uint32_t mountGoh = player ? MountOf(player) : 0;
    g_localVehicle = VehicleOfGoh(mountGoh);
    const uint64_t assigned = api.get_assigned_vehicle ? api.get_assigned_vehicle() : 0;
    if (assigned != g_lastAssigned) {
        if (assigned && g_localVehicle != assigned) {
            if (const uint32_t goh = VehicleGoh(assigned); goh && player) {
                MountActor(player, goh);
                Log("vehicles: server put us on vehicle %llu (horse goh 0x%08x)", static_cast<unsigned long long>(assigned), goh);
                g_pendingMount = 0;
            } else {
                g_pendingMount = assigned;
            }
        }
        if (!assigned && g_lastAssigned && g_localVehicle == g_lastAssigned && player) {
            DismountActor(player);
            Log("vehicles: server took us off vehicle %llu", static_cast<unsigned long long>(g_lastAssigned));
        }
        g_lastAssigned = assigned;
        g_mismatchSince = 0;
    }
    if (g_pendingMount && g_pendingMount == assigned && player && !mountGoh) {
        if (const uint32_t goh = VehicleGoh(assigned)) {
            MountActor(player, goh);
            g_pendingMount = 0;
        }
    }
    // Mounted (with the game's controls) on a vehicle the server gave someone else: it does not confirm it.
    if (g_localVehicle && g_localVehicle != assigned) {
        if (!g_mismatchSince) g_mismatchSince = nowMs;
        else if (nowMs - g_mismatchSince > 4500.0 && player) {
            DismountActor(player);
            Log("vehicles: vehicle %llu is not ours (the server did not confirm it): getting off",
                static_cast<unsigned long long>(g_localVehicle));
            g_mismatchSince = 0;
        }
    } else {
        g_mismatchSince = 0;
    }
}

}  // namespace

void VehiclesForget() {
    g_horses.clear();
}

void LoadVehicleConfig(const wchar_t* ini) {
    g_streamIn = static_cast<float>(GetPrivateProfileIntW(L"net", L"stream_in", 150, ini));
    g_streamOut = static_cast<float>(GetPrivateProfileIntW(L"net", L"stream_out", 200, ini));
    if (g_streamOut < g_streamIn) g_streamOut = g_streamIn;
}

void MountActor(uint32_t riderGoh, uint32_t horseGoh) { CallU("ACTOR_MOUNT_ACTOR", {riderGoh, horseGoh}); }
void DismountActor(uint32_t riderGoh) { CallU("ACTOR_DISMOUNT_NOW", {riderGoh}); }
uint32_t MountOf(uint32_t riderGoh) {
    NativeCall c;
    c.Uint(riderGoh);
    if (!c.Call(NativeHash("IS_ACTOR_RIDING")) || !c.ResultBool()) return 0;
    return CallU("GET_MOUNT", {riderGoh});
}

uint64_t LocalVehicle() { return g_localVehicle; }

uint32_t VehicleGoh(uint64_t netId) {
    const auto it = g_horses.find(netId);
    return it != g_horses.end() && !it->second.hidden ? it->second.goh : 0;
}

void VehiclesFrame(const fw::Api& api, const Vec3& localPos, double nowMs, uint32_t interpMs, float leadMs) {
    if (!api.get_vehicles) return;
    static fw::Vehicle list[512];
    const int n = api.get_vehicles(list, static_cast<int>(std::size(list)));
    const ULONGLONG now = GetTickCount64();

    for (int i = 0; i < n; ++i) {
        const fw::Vehicle& v = list[i];
        Horse& h = g_horses[v.netId];
        h.lastSeen = now;
        h.model = v.model;
        // network state into the timeline (only someone else's: ours is written by us)
        if (!v.mine) {
            PlayerState s;
            s.seq = v.timeMs;
            s.timeMs = v.timeMs ? v.timeMs : 1;
            s.x = v.x, s.y = v.y, s.z = v.z;
            s.heading = v.heading;
            s.speed = v.speed;
            s.input = v.input;
            if (!v.timeMs) {  // server-owned (nobody rides it): one state, numbered by its content
                s.seq = static_cast<uint32_t>(nowMs);
                s.timeMs = 0;
            }
            AddSample(h.timeline, s, nowMs);
        }

        const float dx = v.x - localPos.x, dz = v.z - localPos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (h.goh && !h.hidden && dist > g_streamOut) {
            StreamOutPuppet(h.goh);
            h.hidden = true;
            continue;
        }
        if (dist > g_streamIn && (!h.goh || h.hidden)) continue;
        if (!h.goh) {
            h.goh = SpawnActorOfDef(v.model, 0, Vec3{v.x, v.y, v.z}, v.heading, "vehicle");
            if (!h.goh) continue;
            Log("vehicles: vehicle %llu model %u -> horse goh 0x%08x", static_cast<unsigned long long>(v.netId), v.model, h.goh);
            CallU("TASK_CLEAR", {h.goh});  // no animal AI: it moves only as its rider or the network says
        } else if (h.hidden) {
            if (!StreamInPuppet(h.goh, Vec3{v.x, v.y, v.z}, v.heading)) continue;
            h.hidden = false;
        }
        h.hidden = false;

        const uintptr_t actor = ActorFromGohHandle(h.goh);
        if (v.mine && g_localVehicle == v.netId) {
            // We ride it: the game moves it from our input. Report its pose and the input our riding gives it.
            if (g_ridingActorWatch != h.goh) {
                g_ridingActorWatch = h.goh;
                if (actor) ClearPuppetDesire(actor);
                SetLocoMountActor(actor);
            }
            Vec3 pos{};
            float heading = 0.0f;
            if (!GetPuppetPose(h.goh, pos, heading)) continue;
            if (g_lastOwnMs > 0 && nowMs > g_lastOwnMs + 0.5) {
                const float sx = pos.x - g_lastOwnPos.x, sz = pos.z - g_lastOwnPos.z;
                g_ownSpeed = 0.8f * g_ownSpeed + 0.2f * std::sqrt(sx * sx + sz * sz) * 1000.0f / static_cast<float>(nowMs - g_lastOwnMs);
            }
            g_lastOwnPos = pos;
            g_lastOwnMs = nowMs;
            MoveInput input;
            uint8_t desire[kDesireSize];
            if (GetMountDesire(desire, nullptr)) input = MoveInputFromDesire(desire);
            const float p[3] = {pos.x, pos.y, pos.z};
            api.set_vehicle(v.netId, p, heading, g_ownSpeed, &input);
            continue;
        }
        if (g_ridingActorWatch == h.goh) {  // we got off: back to the network state
            g_ridingActorWatch = 0;
            SetLocoMountActor(0);
            g_lastOwnMs = 0;
        }
        // Someone else's (or nobody's): the puppet controller drives it from the network, the rider's input to it
        // when ridden, standing still when not.
        Remote& r = h.timeline;
        const double target = OffsetMin(r);
        if (!r.haveOffset || r.transit.empty()) r.offset = target;
        else r.offset += (target - r.offset) * 0.02;
        r.haveOffset = !r.transit.empty();
        PuppetTarget t;
        uint8_t controls[PlayerState::kControlCount];
        if (!Interpolate(r.samples, nowMs - interpMs - r.offset, t.pos, t.heading, t.input, controls, t.vel)) continue;
        if (!v.occupant) {
            t.input = MoveInput{};  // nobody rides it: it stands (no AI)
            t.vel = Vec3{0.0f, 0.0f, 0.0f};
        }
        t.leadMs = leadMs;
        DrivePuppet(h.goh, h.drive, t, nowMs);
    }

    // vehicles that are gone (destroyed, or no longer streamed to us)
    for (auto it = g_horses.begin(); it != g_horses.end();) {
        if (now - it->second.lastSeen > 3000) {
            if (it->second.goh && !it->second.hidden) StreamOutPuppet(it->second.goh);
            if (g_ridingActorWatch == it->second.goh) {
                g_ridingActorWatch = 0;
                SetLocoMountActor(0);
            }
            Log("vehicles: vehicle %llu gone", static_cast<unsigned long long>(it->first));
            it = g_horses.erase(it);
        } else {
            ++it;
        }
    }

    LocalMountFrame(api, nowMs);
}

}  // namespace rdmp
