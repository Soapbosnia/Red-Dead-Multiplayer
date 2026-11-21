// MTA-style network client inside the game (rdmp.ini [net]), on the MafiaHub Framework.
//
// Everything runs on the game's main thread, once per rendered frame (AddFrameCallback): the framework client in
// rdmp_client.dll is pumped here (so client scripts run on the main thread and can call game natives directly),
// the local player's state goes up, remote states come into per-player sample buffers timed by the sender's capture
// clock, one puppet per remote player is driven (locomotion desire + per-frame pull) `interp_ms` behind the newest
// samples plus `lead_ms` prediction, and the server's blips are applied.
#include "netclient.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "shared/fw_api.h"
#include "shared/types.h"
#include "blips.h"
#include "host.h"
#include "locomotion.h"
#include "puppet_control.h"
#include "puppet_player.h"
#include "remote_timeline.h"
#include "vehicles.h"
#include "log.h"
#include "mainthread.h"
#include "sandbox.h"

namespace rdmp {
namespace {

using namespace rdmp::net;

// Main thread only (the framework client is pumped from the frame callback too). The mutex stays as a cheap guard.
std::mutex g_mutex;
std::map<uint16_t, Remote> g_remotes;
PlayerState g_local{};
bool g_localValid = false;
float g_localHealth = 100.0f;
bool g_teleportPending = false;  // framework backend: the server moved our avatar
Vec3 g_teleportTo{};
// Script blips: the replicated set, refreshed every frame from the framework client.
std::vector<fw::Blip> g_blips;
uint64_t g_localNetId = 0;

// Config
bool g_enabled = false;
char g_host[64] = "127.0.0.1";
uint16_t g_port = kDefaultPort;
char g_name[kNameLen] = "player";
uint32_t g_interpMs = 30;
float g_streamIn = 150.0f;   // remote players closer than this (m, horizontal) get an actor
float g_streamOut = 200.0f;  // ...and lose it beyond this (hysteresis)
wchar_t g_fwDir[MAX_PATH] = L"";  // folder of rdmp_client.dll + the framework runtime; default: next to rdmp.dll
char g_fwLogDir[MAX_PATH] = "";
char g_fwCacheDir[MAX_PATH] = "";

// Main-thread only
struct Puppet {
    uint32_t goh = 0;
    bool hidden = false;
    int corrections = 0;
    PuppetDrive drive;
    bool playerFailed = false;
    uint32_t mountedOn = 0;     // the horse (goh) the puppet sits on, 0 = on foot  // player control could not be set up: movement request instead
    float testHeading = 0.0f;  // [debug] puppet_player_test
    Vec3 testPos{};
    float testHeadingDeg = 0.0f;
};
bool g_puppetPlayerTest = false;
// rdmp.ini [net] puppet_mode: player (default) = the game's player code drives puppets from the remote's controller state
// (MTA's model); desire = our controller steers the remote's movement request.
bool g_puppetPlayerMode = true;
// rdmp.ini [net] lead_ms: velocity prediction (ms) that cancels the delay of the network position. -1 = auto (default):
// interp_ms + the estimated sender->us delay (both pings / 2 + one server tick, see LeadMsFor).
int g_leadMs = -1;
fw::Link g_link{};  // our ping + the server tick

// Prediction (ms) for a remote = how old its network position is when drawn: the interp buffer plus the delay from the
// sender's game frame to us. That delay = half of each side's round trip to the server + one server tick (the server
// forwards an update on its next tick) + ~2 ms polling; measured on one PC at 120 Hz: fastest transit ~11 ms = this
// model. The network library's ping reads ~10 ms even on localhost (its threads wake every 10 ms; our updates go out
// immediately), so 10 ms of each ping isn't travel time. Before the first link report: ~10 ms.
float LeadMsFor(const Remote& r) {
    if (g_leadMs >= 0) return static_cast<float>(g_leadMs);
    float delay = 10.0f;
    if (g_link.tickMs > 0.0f) {
        const float travel = (std::max(g_link.pingMs - 10, 0) + std::max(r.pingMs - 10, 0)) * 0.5f;
        delay = travel + g_link.tickMs + 2.0f;
    }
    return static_cast<float>(g_interpMs) + delay;
}
std::map<uint16_t, Puppet> g_puppetsById;

// ---- the framework client: rdmp_client.dll (C table, shared/fw_api.h), pumped once per frame ----
fw::Api g_api{};
bool g_fwUp = false;
fw::Conn g_conn = fw::Conn::Connecting;
ULONGLONG g_nextStats = 0, g_nextRetry = 0;
// rdmp.ini [net] autoconnect: 1 = connect to [net] server at start and keep retrying (development); 0 (default) = the
// client's main menu (server browser) decides where to connect.
bool g_autoConnect = false;
uint32_t g_seq = 0, g_samples = 0, g_lastSentTime = 0;

void FwLog(const char* line) { Log("%s", line); }

bool FwStart() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\rdmp_client.dll", g_fwDir);
    // Altered search path: the framework's runtime DLLs (libnode, ...) are found next to rdmp_client.dll.
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        Log("net: can't load %ls (error %lu)", path, GetLastError());
        return false;
    }
    auto getApi = reinterpret_cast<rdmp_client_get_api_fn>(GetProcAddress(mod, "rdmp_client_get_api"));
    if (!getApi || !getApi(&g_api) || g_api.version != fw::kApiVersion) {
        Log("net: rdmp_client.dll API mismatch (want v%u, got v%u): rebuild rdmp.dll and rdmp_client.dll together",
            fw::kApiVersion, g_api.version);
        return false;
    }
    if (!g_api.init(g_fwLogDir, g_fwCacheDir, GameHost())) {
        Log("net: framework client init failed");
        return false;
    }
    if (g_autoConnect) {
        Log("net: framework client up, connecting to %s:%u as \"%s\"", g_host, g_port, g_name);
        g_api.connect(g_host, g_port, g_name);
    } else {
        Log("net: framework client up; the main menu picks the server");
    }
    return true;
}

// One framework pump: our state up, server state down. Called exactly once per frame from NetFrame.
void FwStep() {
    if (!g_fwUp) return;
    HostFrame();
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard lock(g_mutex);
        if (g_localValid && g_local.timeMs != g_lastSentTime) {  // only new frames get a new seq
            g_lastSentTime = g_local.timeMs;
            PlayerState s = g_local;
            s.seq = ++g_seq;
            g_api.set_local(&s, g_localHealth);
        }
    }
    const fw::Conn conn = g_api.update();
    if (conn != g_conn) {
        Log("net: connection %s", conn == fw::Conn::Connected ? "established" : conn == fw::Conn::Connecting ? "connecting" : "closed");
        if (g_conn == fw::Conn::Connected && conn != fw::Conn::Connected) {
            ScriptLeaveSession();  // back to limbo behind the main menu
        }
        g_conn = conn;
    }
    if (g_autoConnect && conn == fw::Conn::Disconnected && now >= g_nextRetry) {  // server down / dropped: retry every 5 s
        g_nextRetry = now + 5000;
        g_api.connect(g_host, g_port, g_name);
    }
    fw::Action action;
    while (g_api.poll_action(&action)) {
        switch (action.kind) {
        case fw::ActionKind::Spawn: ScriptSpawnPlayer(Vec3{action.a, action.b, action.c}, action.d); break;
        case fw::ActionKind::Fade: ScriptFadeCamera(action.a != 0.0f, action.b); break;
        case fw::ActionKind::CameraTarget: ScriptSetCameraTarget(); break;
        case fw::ActionKind::Control: ScriptSetPlayerControl(action.a != 0.0f); break;
        case fw::ActionKind::Health: ScriptSetPlayerHealth(action.a); break;
        case fw::ActionKind::Kill: GameHost()->kill_local(); break;
        case fw::ActionKind::HudText: GameHost()->show_text(static_cast<int>(action.a), action.text, static_cast<int>(action.b)); break;
        default: break;
        }
    }
    fw::Forced forced;
    if (g_api.poll_forced(&forced)) {
        std::lock_guard lock(g_mutex);
        g_teleportTo = Vec3{forced.x, forced.y, forced.z};
        g_teleportPending = true;
    }
    static fw::Remote remotes[kMaxPlayers * 4];
    const int n = g_api.get_remotes(remotes, static_cast<int>(std::size(remotes)));
    fw::Link link;
    g_api.get_link(&link);
    static fw::Blip blips[256];
    const int nBlips = g_api.get_blips(blips, static_cast<int>(std::size(blips)));
    const uint64_t localNetId = g_api.get_local_id();
    std::lock_guard lock(g_mutex);
    g_link = link;
    g_blips.assign(blips, blips + (nBlips > 0 ? nBlips : 0));
    g_localNetId = localNetId;
    for (int i = 0; i < n; ++i) {
        const fw::Remote& e = remotes[i];
        Remote& r = g_remotes[static_cast<uint16_t>(e.netId)];
        if (r.name != e.name) r.name = e.name;
        r.lastSeenMs = now;  // streamed to us = alive; samples only on new owner updates
        r.pingMs = e.pingMs;
        if (AddSample(r, e.s, NowMs())) ++g_samples;
    }
    if (now >= g_nextStats) {
        g_nextStats = now + 10000;
        Log("net: %s, %d remote player(s) streamed, %d blip(s), %u samples, sent %u states",
            conn == fw::Conn::Connected ? "connected" : "not connected", n, nBlips, g_samples, g_seq);
    }
}



// Sync trace ([debug] net_trace=1): CSV per game copy (rdmp_trace[_N].csv) with a shared high-resolution clock, so the
// traces of two copies on one PC can be compared (tools/sync_report.py):
//   L,t_ms,x,y,z,heading,speed,moving,desire words,C:controls(hex),H:control heading   (this copy's player, every frame)
//   R,t_ms,id,tx,ty,tz,theading,px,py,pz,pheading,err,moving,tp,pulled,transit,offset,lead (each remote: network
//     target, puppet before this frame's correction, gap to the predicted present, teleported, soft pull applied)
FILE* g_trace = nullptr;
double TraceMs() { return NowMs(); }

void NetFrame(float /*gameTime*/) {
    // Connect as soon as the world is up, even with no player yet: in script mode the player is created
    // by the server's spawnPlayer call, so waiting for one would deadlock the join.
    static bool started = false;
    if (!started && IsWorldReady()) {
        started = true;
        g_fwUp = FwStart();
    }

    Vec3 pos{};
    float heading = 0.0f;
    if (!GetLocalPlayerState(pos, heading) || (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f)) {
        FwStep();  // no player yet (or its transform is not set up): keep the connection running
        return;
    }
    const double traceT = g_trace ? TraceMs() : 0.0;

    static double lastMs = 0;
    static Vec3 lastPos{};
    static float speed = 0.0f;
    const ULONGLONG now = GetTickCount64();
    const double nowMs = NowMs();
    if (lastMs > 0 && nowMs > lastMs + 0.5) {
        const float dx = pos.x - lastPos.x, dz = pos.z - lastPos.z;
        const float inst = std::sqrt(dx * dx + dz * dz) * 1000.0f / static_cast<float>(nowMs - lastMs);
        speed = 0.8f * speed + 0.2f * inst;
    }
    lastMs = nowMs;
    lastPos = pos;

    struct View {
        uint16_t id;
        std::string name;
        Vec3 pos;
        float heading;
        bool stale;
        MoveInput input;
        uint8_t controls[PlayerState::kControlCount];
        uint64_t vehicle;  // the vehicle the server has them on (newest state), 0 = on foot
        Vec3 vel;
        float transit;  // newest sample: arrival - sender capture time (ms; the real delay when both clocks are one PC's)
        float offset;   // sender clock -> local clock mapping in use (ms)
        float leadMs;   // prediction that cancels the network position's delay
    };
    View views[kMaxPlayers];
    int nViews = 0;
    {
        std::lock_guard lock(g_mutex);
        g_local.x = pos.x;
        g_local.y = pos.y;
        g_local.z = pos.z;
        g_local.heading = heading;
        g_local.speed = speed;
        g_local.timeMs = static_cast<uint32_t>(static_cast<uint64_t>(nowMs));
        if (!g_local.timeMs) g_local.timeMs = 1;  // 0 = "no clock"
        static uint8_t desire[kDesireSize];
        if (GetPlayerDesire(desire, nullptr)) g_local.input = MoveInputFromDesire(desire);
        PlayerControls controls;
        g_local.vehicle = LocalVehicle();
        if (CaptureLocalControls(controls)) std::memcpy(g_local.controls, controls.value, sizeof(g_local.controls));
        float health = 0.0f;
        if (GetLocalPlayerHealth(health)) g_localHealth = health;
        g_localValid = true;
        if (g_trace) {
            fprintf(g_trace, "L,%.2f,%.3f,%.3f,%.3f,%.2f,%.3f,%d,", traceT, pos.x, pos.y, pos.z, heading, speed, (g_local.input.gait & 0x0f) ? 1 : 0);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(desire);  // raw desire: every non-zero word (idx:hex)
            for (int i = 0; i < kDesireWords; ++i)
                if (w[i]) fprintf(g_trace, "%02x:%08x ", i, w[i]);
            // controller state (30 input values) and the control heading, for replays in player mode
            fprintf(g_trace, ",C:");
            for (uint8_t c : g_local.controls) fprintf(g_trace, "%02x", c);
            fprintf(g_trace, ",H:%.4f\n", controls.valid ? controls.heading : 0.0f);
        }

        if (g_teleportPending) {  // server override of our position (framework backend)
            g_teleportPending = false;
            TeleportLocalPlayer(g_teleportTo, heading);
            Log("net: server teleported us to (%.2f, %.2f, %.2f)", g_teleportTo.x, g_teleportTo.y, g_teleportTo.z);
        }
        const double renderT = nowMs - static_cast<double>(g_interpMs);
        for (auto it = g_remotes.begin(); it != g_remotes.end() && nViews < kMaxPlayers;) {
            Remote& r = it->second;
            const bool stale = now - r.lastSeenMs > 3000;
            // sender clock -> ours: follow the fastest recent transit (eased, so the timeline never jumps)
            const double target = OffsetMin(r);
            if (!r.haveOffset || r.transit.empty()) r.offset = target;
            else r.offset += (target - r.offset) * 0.02;
            r.haveOffset = !r.transit.empty();
            View& v = views[nViews];
            if (Interpolate(r.samples, renderT - r.offset, v.pos, v.heading, v.input, v.controls, v.vel)) {
                v.id = it->first;
                v.name = r.name;
                v.stale = stale;
                v.transit = r.samples.empty() ? -1.0f : static_cast<float>(r.samples.back().arrival - r.samples.back().t);
                v.offset = static_cast<float>(r.offset);
                v.leadMs = LeadMsFor(r);
                v.vehicle = r.samples.empty() ? 0 : r.samples.back().s.vehicle;
                ++nViews;
            }
            ++it;
        }
    }
    FwStep();  // hand the new local state to the network now, take the server's
    VehiclesFrame(g_api, pos, nowMs, g_interpMs, static_cast<float>(g_interpMs) + 20.0f);

    // Script blips: reconcile the engine's blips with the server's set. An attached blip follows the live position
    // of its element when we have it (the local player, or a remote player's interpolated position), else the
    // server's copy of the target's position (a remote beyond our stream range).
    {
        static std::vector<WantedBlip> wanted;
        uint64_t localNetId = 0;
        {
            std::lock_guard lock(g_mutex);
            wanted.clear();
            wanted.reserve(g_blips.size());
            for (const fw::Blip& b : g_blips) {
                WantedBlip w;
                w.id = b.netId;
                w.pos = Vec3{b.x, b.y, b.z};
                w.attachedTo = b.attachedTo;
                w.icon = b.icon;
                w.rgba = b.rgba;
                w.scale = b.scale;
                w.maxDistance = b.maxDistance;
                w.priority = b.priority;
                w.flags = b.flags;
                wanted.push_back(w);
            }
            localNetId = g_localNetId;
        }
        struct Resolver {
            uint64_t localId;
            Vec3 localPos;
            const View* views;
            int count;
        } res{localNetId, pos, views, nViews};
        ApplyBlips(wanted.data(), static_cast<int>(wanted.size()),
                   [](uint64_t netId, Vec3& out, void* user) -> bool {
                       const auto* r = static_cast<const Resolver*>(user);
                       if (r->localId && netId == r->localId) {
                           out = r->localPos;
                           return true;
                       }
                       for (int i = 0; i < r->count; ++i) {
                           if (r->views[i].id == static_cast<uint16_t>(netId) && !r->views[i].stale) {
                               // where the remote is drawn: its puppet when it has one, else the network position
                               const auto pit = g_puppetsById.find(r->views[i].id);
                               if (pit != g_puppetsById.end() && pit->second.goh && !pit->second.hidden &&
                                   GetPuppetPosition(pit->second.goh, out))
                                   return true;
                               out = r->views[i].pos;
                               return true;
                           }
                       }
                       return false;
                   },
                   &res);
    }

    for (int i = 0; i < nViews; ++i) {
        const View& v = views[i];
        Puppet& p = g_puppetsById[v.id];
        // Distance streaming (MTA-style): a remote player only has an actor while within stream_in m of us (horizontal);
        // beyond stream_out m (or timed out) the actor is released (goh kept, re-activated when back in range).
        const float sdx = v.pos.x - pos.x, sdz = v.pos.z - pos.z;
        const float dist = std::sqrt(sdx * sdx + sdz * sdz);
        if (v.stale || (p.goh && !p.hidden && dist > g_streamOut)) {
            if (p.goh && !p.hidden) {
                DetachPlayerControl(p.goh);
                StreamOutPuppet(p.goh);
                p.hidden = true;
                Log("net: player %u \"%s\" streamed out (%s, %.0f m)", v.id, v.name.c_str(), v.stale ? "timed out" : "far", dist);
            }
            continue;
        }
        if (dist > g_streamIn && (!p.goh || p.hidden)) continue;  // out of range: no actor
        if (!p.goh) {
            p.goh = SpawnRemotePuppet(v.pos, v.heading, v.name.c_str());
            Log("net: player %u \"%s\" -> puppet goh 0x%08x at (%.2f, %.2f, %.2f), %.0f m away", v.id, v.name.c_str(), p.goh,
                v.pos.x, v.pos.y, v.pos.z, dist);
            if (!p.goh) continue;
        } else if (p.hidden) {
            if (!StreamInPuppet(p.goh, v.pos, v.heading)) continue;
            Log("net: player %u \"%s\" streamed in (%.0f m)", v.id, v.name.c_str(), dist);
        }
        p.hidden = false;
        // On a horse: the horse carries the puppet (no player control, no corrections of its own).
        if (const uint32_t horse = v.vehicle ? VehicleGoh(v.vehicle) : 0) {
            if (p.mountedOn != horse) {
                DetachPlayerControl(p.goh);
                if (const uintptr_t a = ActorFromGohHandle(p.goh)) ClearPuppetDesire(a);
                MountActor(p.goh, horse);
                p.mountedOn = horse;
                Log("net: player %u mounts horse goh 0x%08x", v.id, horse);
            }
            continue;
        }
        if (p.mountedOn) {
            DismountActor(p.goh);
            p.mountedOn = 0;
            p.drive = PuppetDrive{};
            Log("net: player %u dismounts", v.id);
        }
        if (g_puppetPlayerTest) {
            // [debug] puppet_player_test: the puppet is driven by the game's player code from a fixed input pattern
            // (8 s loop: 3 s stick forward, 1 s nothing, the 0x18 input pressed for 0.2 s, rest), no network input.
            if (!HasPlayerControl(p.goh)) {
                if (!AttachPlayerControl(p.goh)) continue;
                // 6 m in front of the local player, so the test is in view
                const float hr = heading * 0.01745329f;
                SetPuppetTransform(p.goh, Vec3{pos.x - std::sin(hr) * 6.0f, pos.y + 0.3f, pos.z - std::cos(hr) * 6.0f}, heading + 90.0f);
            }
            PlayerControls c;
            c.valid = true;
            c.value[0x11] = c.value[0x12] = 0x7f;
            const double t = std::fmod(nowMs / 1000.0, 8.0);
            if (t < 3.0) c.value[0x12] = 0x00;            // stick forward
            if (t >= 4.0 && t < 4.2) c.value[0x18] = 0xff;  // jump / sprint input
            if (t >= 5.0 && t < 7.0) c.value[0x12] = 0x00, c.value[0x18] = (std::fmod(t, 0.4) < 0.2) ? 0xff : 0;  // tapped: sprint
            // walk across the local player's view, back the other way every second loop
            const bool back = static_cast<int64_t>(nowMs / 8000.0) % 2 != 0;
            c.heading = std::fmod(heading * 0.01745329f + 3.14159265f + (back ? -1.5707963f : 1.5707963f) + 6.2831853f, 6.2831853f);
            SetPlayerControls(p.goh, c);
            continue;
        }
        PuppetTarget target;
        target.pos = v.pos;
        target.vel = v.vel;
        target.heading = v.heading;
        target.leadMs = v.leadMs;
        target.input = v.input;
        if (g_puppetPlayerMode) {
            // MTA-style: the game's own player code moves the puppet from the remote's controller state
            if (!HasPlayerControl(p.goh) && !p.playerFailed) {
                p.playerFailed = !AttachPlayerControl(p.goh);
                if (p.playerFailed) Log("net: player %u: no player control, falling back to the movement request", v.id);
            }
            if (HasPlayerControl(p.goh)) {
                PlayerControls c;
                std::memcpy(c.value, v.controls, sizeof(c.value));
                c.heading = v.input.heading;
                c.valid = true;
                SetPlayerControls(p.goh, c);
                target.playerDriven = true;
            }
        }
        const PuppetDriveResult d = DrivePuppet(p.goh, p.drive, target, nowMs);
        if (d.teleported && d.err >= 0.0f && ++p.corrections <= 20)
            Log("net: player %u drift %.2f m (%s) -> teleport", v.id, d.err, d.moving ? "moving" : "idle");
        if (g_trace)
            fprintf(g_trace, "R,%.2f,%u,%.3f,%.3f,%.3f,%.2f,%.3f,%.3f,%.3f,%.2f,%.3f,%d,%d,%d,%.2f,%.2f,%.1f\n", traceT, v.id, v.pos.x,
                    v.pos.y, v.pos.z, v.heading, d.cur.x, d.cur.y, d.cur.z, d.curHeading, d.err, d.moving ? 1 : 0,
                    d.teleported ? 1 : 0, d.corrected ? 1 : 0, v.transit, v.offset, v.leadMs);
    }
    PuppetPlayerFrame();
    if (g_trace) fflush(g_trace);  // test runs end by killing the game
}

}  // namespace

void StartNetClient(const wchar_t* ini) {
    g_enabled = GetPrivateProfileIntW(L"net", L"enabled", 0, ini) != 0;
    if (!g_enabled) return;
    wchar_t w[64];
    GetPrivateProfileStringW(L"net", L"server", L"127.0.0.1", w, 64, ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_host, sizeof(g_host), nullptr, nullptr);
    g_port = static_cast<uint16_t>(GetPrivateProfileIntW(L"net", L"port", kDefaultPort, ini));
    GetPrivateProfileStringW(L"net", L"name", L"player", w, 64, ini);
    // RDMP.exe --name X overrides the ini; extra game copies (--instance N) get the instance number appended.
    if (GetEnvironmentVariableW(L"RDMP_NAME", w, 64) == 0 && InstanceId() > 1) {
        const size_t len = wcslen(w);
        swprintf_s(w + len, 64 - len, L"%d", InstanceId());
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_name, sizeof(g_name), nullptr, nullptr);
    g_name[kNameLen - 1] = '\0';
    g_autoConnect = GetPrivateProfileIntW(L"net", L"autoconnect", 0, ini) != 0;
    g_interpMs = GetPrivateProfileIntW(L"net", L"interp_ms", 30, ini);
    g_streamIn = static_cast<float>(GetPrivateProfileIntW(L"net", L"stream_in", 150, ini));
    g_streamOut = static_cast<float>(GetPrivateProfileIntW(L"net", L"stream_out", 200, ini));
    if (g_streamOut < g_streamIn) g_streamOut = g_streamIn;
    LoadPuppetControlConfig(ini);
    LoadVehicleConfig(ini);
    g_puppetPlayerTest = GetPrivateProfileIntW(L"debug", L"puppet_player_test", 0, ini) != 0;
    GetPrivateProfileStringW(L"net", L"puppet_mode", L"player", w, 64, ini);
    g_puppetPlayerMode = _wcsicmp(w, L"desire") != 0;
    GetPrivateProfileStringW(L"net", L"lead_ms", L"auto", w, 64, ini);
    g_leadMs = (_wcsicmp(w, L"auto") == 0 || !*w) ? -1 : _wtoi(w);
    // Everything lives next to rdmp.ini / rdmp.dll (the build's bin folder): the client DLL and framework runtime,
    // the framework's logs (logs\) and the resources it downloads from servers (cache\). [net] fw_dir overrides
    // the DLL folder only.
    wchar_t binDir[MAX_PATH];
    wcscpy_s(binDir, ini);
    if (wchar_t* slash = wcsrchr(binDir, L'\\')) *slash = L'\0';
    GetPrivateProfileStringW(L"net", L"fw_dir", binDir, g_fwDir, MAX_PATH, ini);
    if (!g_fwDir[0]) wcscpy_s(g_fwDir, binDir);
    wchar_t sub[MAX_PATH];
    swprintf_s(sub, L"%s\\logs", binDir);
    WideCharToMultiByte(CP_UTF8, 0, sub, -1, g_fwLogDir, sizeof(g_fwLogDir), nullptr, nullptr);
    swprintf_s(sub, L"%s\\cache", binDir);
    WideCharToMultiByte(CP_UTF8, 0, sub, -1, g_fwCacheDir, sizeof(g_fwCacheDir), nullptr, nullptr);
    if (GetPrivateProfileIntW(L"debug", L"net_trace", 0, ini)) {
        wchar_t tracePath[MAX_PATH];
        wcscpy_s(tracePath, ini);
        if (wchar_t* slash = wcsrchr(tracePath, L'\\')) {
            wchar_t name[40];
            if (InstanceId() > 1) swprintf_s(name, L"rdmp_trace_%d.csv", InstanceId());
            else wcscpy_s(name, L"rdmp_trace.csv");
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - tracePath), name);
        }
        g_trace = _wfsopen(tracePath, L"wN", _SH_DENYWR);
        Log("net: sync trace %s", g_trace ? "on" : "FAILED to open");
    }
    AddFrameCallback(&NetFrame);  // starts the framework client once the world is ready
    Log("net: enabled (server %s:%u, name \"%s\", client %ls, interp %u ms, stream in %.0f / out %.0f m)", g_host, g_port,
        g_name, g_fwDir, g_interpMs, g_streamIn, g_streamOut);
}

bool RemotePuppetPosition(unsigned long long netId, float out[3]) {
    const auto it = g_puppetsById.find(static_cast<uint16_t>(netId));
    if (it == g_puppetsById.end() || !it->second.goh || it->second.hidden) return false;
    Vec3 p{};
    if (!GetPuppetPosition(it->second.goh, p)) return false;
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
    return true;
}

}  // namespace rdmp
