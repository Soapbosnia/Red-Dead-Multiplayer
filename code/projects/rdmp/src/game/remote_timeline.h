#pragma once
// The timeline of a remote element (player or vehicle): its received states, placed by the sender's capture time and
// interpolated interp_ms behind the newest (see netclient.cpp). Main thread.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>

#include "actors.h"
#include "shared/types.h"

namespace rdmp {

using net::MoveInput;
using net::PlayerState;

// High-resolution local clock (ms). GetTickCount64 only ticks every ~15.6 ms, which is too coarse to time samples and
// interpolation (it also made the speed estimate wrong by ~2x).
inline double NowMs() {
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
inline bool AddSample(Remote& r, const PlayerState& s, double arrival) {
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
inline double OffsetMin(const Remote& r) {
    double m = 1e300;
    for (const auto& e : r.transit) m = std::min(m, e.second);
    return r.transit.empty() ? 0.0 : m;
}


inline float LerpAngleDeg(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return a + d * t;
}

// Position at render time: linear interpolation between the two samples around it (clamped to the newest), plus the
// remote's velocity (m/s, from the surrounding samples) for path-following.
inline bool Interpolate(const std::deque<Sample>& samples, double renderT, Vec3& pos, float& heading, MoveInput& input, uint8_t* controls, Vec3& vel) {
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
    std::memcpy(controls, b->s.controls, sizeof(b->s.controls));
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

}  // namespace rdmp
