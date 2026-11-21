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
#include "log.h"
#include "mainthread.h"
#include "sandbox.h"

namespace rdmp {
namespace {

using namespace rdmp::net;

// High-resolution local clock (ms). GetTickCount64 only ticks every ~15.6 ms, which is too coarse to time samples and
// interpolation (it also made the speed estimate wrong by ~2x).
double NowMs() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// Timeline: samples are placed by the SENDER's capture time (PlayerState.timeMs, unwrapped to ms), not by when they
// happened to arrive, so polling/tick jitter on the way doesn't wobble the interpolation and the buffer can be short.
// The sender's clock is mapped onto ours with the smallest (arrival - send) seen recently = clock offset + fastest
// transit (on one PC the clocks are the same, so that's just the fastest transit).
struct Sample {
    double t;        // sender capture time on the sender's clock (ms); arrival time when the sender has no clock
    double arrival;  // local receive time (NowMs)
    PlayerState s;
};

struct Remote {
    std::string name;
    std::deque<Sample> samples;
    ULONGLONG lastSeenMs = 0;
    uint32_t lastTimeMs = 0;            // last raw sender clock (for unwrapping)
    double offset = 0;                  // in use: local time = sender time + offset (eased toward offsetMin)
    bool haveOffset = false;
    std::deque<std::pair<double, double>> transit;  // (arrival, arrival - send) of the last ~3 s
    int pingMs = 0;                     // that player's round trip to the server
};

// Adds a received state to the remote's buffer. False = old/duplicate.
bool AddSample(Remote& r, const PlayerState& s, double arrival) {
    if (!r.samples.empty() && static_cast<int32_t>(s.seq - r.samples.back().s.seq) <= 0) return false;
    double t = arrival;
    if (s.timeMs) {
        t = r.samples.empty() || !r.lastTimeMs ? static_cast<double>(s.timeMs)
                                               : r.samples.back().t + static_cast<int32_t>(s.timeMs - r.lastTimeMs);
        if (!r.samples.empty() && t <= r.samples.back().t) return false;  // same game frame sent twice
        r.lastTimeMs = s.timeMs;
        r.transit.emplace_back(arrival, arrival - t);
        while (r.transit.size() > 2 && arrival - r.transit.front().first > 3000.0) r.transit.pop_front();
    } else {
        r.lastTimeMs = 0;
        r.transit.clear();
    }
    r.samples.push_back(Sample{t, arrival, s});
    while (r.samples.size() > 64) r.samples.pop_front();
    return true;
}

// Smallest (arrival - send) in the window: the sender->local clock mapping with the fastest transit.
double OffsetMin(const Remote& r) {
    double m = 1e300;
    for (const auto& e : r.transit) m = std::min(m, e.second);
    return r.transit.empty() ? 0.0 : m;
}

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
};
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

float LerpAngleDeg(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return a + d * t;
}

// Position at render time: linear interpolation between the two samples around it (clamped to the newest), plus the
// remote's velocity (m/s, from the surrounding samples) for path-following.
bool Interpolate(const std::deque<Sample>& samples, double renderT, Vec3& pos, float& heading, MoveInput& input, Vec3& vel) {
    if (samples.empty()) return false;
    size_t ia = 0, ib = samples.size() - 1;
    if (renderT <= samples[ia].t) {
        ib = ia;
    } else if (renderT < samples[ib].t) {
        for (size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].t >= renderT) {
                ia = i - 1;
                ib = i;
                break;
            }
        }
    } else {
        ia = ib;
    }
    const Sample* a = &samples[ia];
    const Sample* b = &samples[ib];
    const float t = b->t > a->t ? static_cast<float>((renderT - a->t) / (b->t - a->t)) : 1.0f;
    pos = Vec3{a->s.x + (b->s.x - a->s.x) * t, a->s.y + (b->s.y - a->s.y) * t, a->s.z + (b->s.z - a->s.z) * t};
    heading = LerpAngleDeg(a->s.heading, b->s.heading, t);
    input = b->s.input;
    // velocity over a ~100 ms window ending at b (single-sample spans are too jittery)
    size_t iv = ib;
    while (iv > 0 && samples[ib].t - samples[iv].t < 100.0) --iv;
    const double dt = (samples[ib].t - samples[iv].t) / 1000.0;
    vel = dt > 0.02 ? Vec3{static_cast<float>((samples[ib].s.x - samples[iv].s.x) / dt), 0.0f,
                           static_cast<float>((samples[ib].s.z - samples[iv].s.z) / dt)}
                    : Vec3{0.0f, 0.0f, 0.0f};
    // past the newest sample (a late update): keep going along the velocity for up to 100 ms instead of freezing, so a
    // short interp buffer doesn't stutter
    if (ia == ib && ib == samples.size() - 1 && renderT > b->t) {
        const float over = static_cast<float>(std::min(renderT - b->t, 100.0)) / 1000.0f;
        pos.x += vel.x * over;
        pos.z += vel.z * over;
    }
    return true;
}

// The movement input inside the local player's 0x110-byte locomotion desire (shared/types.h MoveInput).
MoveInput InputFromDesire(const uint8_t* desire) {
    MoveInput in;
    std::memcpy(&in.right, desire + 0x14 * 4, 4);
    std::memcpy(&in.back, desire + 0x16 * 4, 4);
    std::memcpy(&in.heading, desire + 0x38 * 4, 4);
    uint32_t gait = 0;
    std::memcpy(&gait, desire + 0x3f * 4, 4);
    in.gait = static_cast<uint8_t>(gait & 0xff);
    if (!(in.heading >= 0.0f && in.heading <= 6.3f)) in.heading = 0.0f;  // not a heading (stick released / garbage)
    return in;
}

// Sync trace ([debug] net_trace=1): CSV per game copy (rdmp_trace[_N].csv) with a shared high-resolution clock, so the
// traces of two copies on one PC can be compared (tools/sync_report.py):
//   L,t_ms,x,y,z,heading,speed,moving                          (this copy's player, every frame)
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
        if (GetPlayerDesire(desire, nullptr)) g_local.input = InputFromDesire(desire);
        float health = 0.0f;
        if (GetLocalPlayerHealth(health)) g_localHealth = health;
        g_localValid = true;
        if (g_trace) {
            fprintf(g_trace, "L,%.2f,%.3f,%.3f,%.3f,%.2f,%.3f,%d,", traceT, pos.x, pos.y, pos.z, heading, speed, (g_local.input.gait & 0x0f) ? 1 : 0);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(desire);  // raw desire: every non-zero word (idx:hex)
            for (int i = 0; i < kDesireWords; ++i)
                if (w[i]) fprintf(g_trace, "%02x:%08x ", i, w[i]);
            fputc('\n', g_trace);
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
            if (Interpolate(r.samples, renderT - r.offset, v.pos, v.heading, v.input, v.vel)) {
                v.id = it->first;
                v.name = r.name;
                v.stale = stale;
                v.transit = r.samples.empty() ? -1.0f : static_cast<float>(r.samples.back().arrival - r.samples.back().t);
                v.offset = static_cast<float>(r.offset);
                v.leadMs = LeadMsFor(r);
                ++nViews;
            }
            ++it;
        }
    }
    FwStep();  // hand the new local state to the network now, take the server's

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
        PuppetTarget target;
        target.pos = v.pos;
        target.vel = v.vel;
        target.heading = v.heading;
        target.leadMs = v.leadMs;
        target.input = v.input;
        const PuppetDriveResult d = DrivePuppet(p.goh, p.drive, target, nowMs);
        if (d.teleported && d.err >= 0.0f && ++p.corrections <= 20)
            Log("net: player %u drift %.2f m (%s) -> teleport", v.id, d.err, d.moving ? "moving" : "idle");
        if (g_trace)
            fprintf(g_trace, "R,%.2f,%u,%.3f,%.3f,%.3f,%.2f,%.3f,%.3f,%.3f,%.2f,%.3f,%d,%d,%d,%.2f,%.2f,%.1f\n", traceT, v.id, v.pos.x,
                    v.pos.y, v.pos.z, v.heading, d.cur.x, d.cur.y, d.cur.z, d.curHeading, d.err, d.moving ? 1 : 0,
                    d.teleported ? 1 : 0, d.corrected ? 1 : 0, v.transit, v.offset, v.leadMs);
    }
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
