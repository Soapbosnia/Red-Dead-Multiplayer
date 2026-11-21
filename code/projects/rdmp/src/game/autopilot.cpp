// Debug autopilot (sync testing): replays a recorded route on the local player of one game copy, so a second copy on
// the same PC can watch the puppet while the moving copy runs in the background (it gets no keyboard input there).
//
// rdmp.ini [debug] autopilot=<trace csv> (relative to rdmp.ini), autopilot_instance=<copy number, default 2>.
// The file is a sync trace ([debug] net_trace=1): its L rows carry the time and the raw locomotion desire words of
// the copy that recorded it. The replay starts autopilot_delay_ms (default 8000) after the player is in the world,
// puts the player where the recording started, feeds each recorded desire at its recorded time and loops after a 3 s
// pause.
#include "autopilot.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "locomotion.h"
#include "puppet_player.h"
#include "log.h"
#include "mainthread.h"
#include "sandbox.h"

namespace rdmp {
namespace {

struct Step {
    double t;  // ms from the first step
    uint8_t desire[kDesireSize];
    PlayerControls controls;  // controller state, when the trace has it (then the replay drives the input instead)
};
bool g_controlsMode = false;

std::vector<Step> g_route;
Vec3 g_startPos{};  // where the recording started (each loop starts there)
float g_startHeading = 0.0f;
double g_startDelayMs = 8000.0;

double NowMs() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

bool LoadRoute(const wchar_t* path) {
    FILE* f = _wfsopen(path, L"r", _SH_DENYNO);
    if (!f) return false;
    char line[4096];
    double t0 = -1.0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != 'L' || line[1] != ',') continue;
        // L,t,x,y,z,heading,speed,moving,<idx:hex ...>
        const char* p = line;
        int comma = 0;
        double t = 0.0;
        const char* words = nullptr;
        for (; *p; ++p) {
            if (*p != ',') continue;
            ++comma;
            if (comma == 1) t = atof(p + 1);
            if (comma == 8) {
                words = p + 1;
                break;
            }
        }
        if (!words) continue;
        if (t0 < 0.0) {
            t0 = t;
            float x = 0, y = 0, z = 0, h = 0;
            if (sscanf_s(line, "L,%*f,%f,%f,%f,%f", &x, &y, &z, &h) == 4) g_startPos = Vec3{x, y, z}, g_startHeading = h;
        }
        Step s{t - t0, {}};
        uint32_t* w = reinterpret_cast<uint32_t*>(s.desire);
        unsigned idx = 0, val = 0;
        int used = 0;
        while (sscanf_s(words, "%x:%x%n", &idx, &val, &used) == 2) {
            if (idx < kDesireSize / 4) w[idx] = val;
            words += used;
        }
        if (const char* cs = strstr(line, ",C:")) {
            cs += 3;
            bool ok = true;
            for (int i = 0; i < kInputCount && ok; ++i) {
                unsigned v = 0;
                ok = sscanf_s(cs + i * 2, "%2x", &v) == 1;
                s.controls.value[i] = static_cast<uint8_t>(v);
            }
            if (const char* hs = strstr(cs, ",H:")) s.controls.heading = static_cast<float>(atof(hs + 3));
            s.controls.valid = ok;
            if (ok) g_controlsMode = true;
        }
        g_route.push_back(s);
    }
    fclose(f);
    return !g_route.empty();
}

void AutopilotFrame(float) {
    static double readyAt = 0.0, startAt = 0.0;
    static size_t next = 0;
    Vec3 pos{};
    float heading = 0.0f;
    if (!IsWorldReady() || !GetLocalPlayerState(pos, heading) || (pos.x == 0.0f && pos.z == 0.0f) || pos.y < -100.0f) {
        readyAt = 0.0;  // not spawned (or parked in limbo): wait for the spawn
        return;
    }
    const double now = NowMs();
    if (!readyAt) {
        readyAt = now;
        startAt = now + g_startDelayMs;
        next = 0;
        Log("autopilot: player in the world, route of %zu steps (%.1f s) starts in %.0f ms", g_route.size(),
            g_route.back().t / 1000.0, g_startDelayMs);
    }
    static bool placed = false;
    if (now < startAt) {
        if (!placed && now > startAt - 2000.0) {  // back to the recorded start, 2 s before the route runs
            placed = true;
            TeleportLocalPlayer(g_startPos, g_startHeading);
        }
        return;
    }
    placed = false;
    const double t = now - startAt;
    while (next + 1 < g_route.size() && g_route[next + 1].t <= t) ++next;
    if (t > g_route.back().t) {
        SetPlayerDesireOverride(nullptr);
        SetLocalControlsOverride(nullptr);
        startAt = now + 3000.0;  // pause, then again from the start
        next = 0;
        Log("autopilot: route done, looping in 3 s");
        return;
    }
    if (g_controlsMode) SetLocalControlsOverride(&g_route[next].controls);
    else SetPlayerDesireOverride(g_route[next].desire);
}

}  // namespace

void StartAutopilot(const wchar_t* ini) {
    wchar_t file[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"debug", L"autopilot", L"", file, MAX_PATH, ini);
    if (!file[0]) return;
    if (InstanceId() != static_cast<int>(GetPrivateProfileIntW(L"debug", L"autopilot_instance", 2, ini))) return;
    std::wstring path = file;
    if (path.size() < 2 || path[1] != L':') {  // relative to rdmp.ini
        std::wstring dir = ini;
        dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
        path = dir + path;
    }
    g_startDelayMs = GetPrivateProfileIntW(L"debug", L"autopilot_delay_ms", 8000, ini);
    if (!LoadRoute(path.c_str())) {
        Log("autopilot: can't read a route from %ls", path.c_str());
        return;
    }
    AddFrameCallback(&AutopilotFrame);
    Log("autopilot: %zu steps from %ls (%s)", g_route.size(), path.c_str(), g_controlsMode ? "controller state" : "movement request");
}

}  // namespace rdmp
