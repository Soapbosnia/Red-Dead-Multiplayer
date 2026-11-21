// Remote-player puppet controller: see puppet_control.h.
//
// Desire words used (RE: re/NOTES.md "Puppet WALK/RUN ANIMATION"): [0x14] local right, [0x15] -0, [0x16] -forward
// (the move vector, relative to the actor's own facing), [0x34] remaining turn (desired - current heading, rad,
// + = left), [0x38] desired world heading + pi (rad), [0x3f] 0xc2280000 | gait (0 stand, 1 walk, 4 sprint).
#include "puppet_control.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "locomotion.h"
#include "log.h"
#include "sandbox.h"

namespace rdmp {
namespace {

using net::MoveDesire;

constexpr float kPi = 3.14159265f;
constexpr float kDeg = 0.01745329f;

bool g_corrections = true;
float g_correctMoving = 0.6f;   // gap (m) that starts a soft pull while the remote moves
float g_correctStanding = 0.12f; // ... and once the puppet has stood still for a moment
constexpr float kCorrectDoneMoving = 0.25f;   // the pull ends below this gap (moving: the animation lags by the delay)
constexpr float kCorrectDoneStanding = 0.04f;
constexpr float kCorrectTau = 0.12f;   // pull time constant (s): ~90% of the gap in a quarter second
constexpr float kTeleport = 4.0f;      // larger gaps are teleported
constexpr float kLevel = 0.5f;         // a height difference above this = the remote is on another level (crate, step)

float WrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
void PutF(uint8_t* d, int word, float v) { std::memcpy(d + word * 4, &v, 4); }
float GetF(const uint8_t* d, int word) {
    float v;
    std::memcpy(&v, d + word * 4, 4);
    return v;
}
void PutU(uint8_t* d, int word, uint32_t v) { std::memcpy(d + word * 4, &v, 4); }
uint32_t GetU(const uint8_t* d, int word) {
    uint32_t v;
    std::memcpy(&v, d + word * 4, 4);
    return v;
}

void Decode(const MoveDesire& in, uint8_t* desire) {
    std::memset(desire, 0, kDesireSize);
    uint32_t* w = reinterpret_cast<uint32_t*>(desire);
    for (int i = 0; i < in.count && i < net::kMaxDesireEntries; ++i)
        if (in.idx[i] < net::kDesireWords) w[in.idx[i]] = in.val[i];
}

void SetGait(uint8_t* d, uint32_t gait) {
    uint32_t w = GetU(d, 0x3f);
    if (!w) w = 0xc2280000u;
    PutU(d, 0x3f, (w & ~0xffu) | gait);
}

// Points the move vector at world direction (ax, az) with stick magnitude `mag`, turning toward it.
void Steer(uint8_t* d, float curHeadingDeg, float ax, float az, float mag) {
    const float len = std::sqrt(ax * ax + az * az);
    if (len < 1e-3f) return;
    const float hp = curHeadingDeg * kDeg;
    const float fwd = (ax * -std::sin(hp) + az * -std::cos(hp)) / len;
    const float right = (ax * std::cos(hp) + az * -std::sin(hp)) / len;
    PutF(d, 0x14, right * mag);
    PutU(d, 0x15, 0x80000000u);
    PutF(d, 0x16, -fwd * mag);
    const float hd = std::atan2(-ax, -az);
    float want = hd + kPi;
    if (want >= 2.0f * kPi) want -= 2.0f * kPi;
    PutF(d, 0x34, WrapPi(hd - hp));
    PutF(d, 0x38, want);
}

void Stand(uint8_t* d) {
    PutF(d, 0x14, 0.0f);
    PutU(d, 0x15, 0u);
    PutF(d, 0x16, 0.0f);
    SetGait(d, 0u);
}

// Turn in place toward headingDeg.
void Face(uint8_t* d, float curHeadingDeg, float headingDeg) {
    const float turn = WrapPi((headingDeg - curHeadingDeg) * kDeg);
    if (std::fabs(turn) < 2.0f * kDeg) return;
    float want = headingDeg * kDeg + kPi;
    while (want < 0.0f) want += 2.0f * kPi;
    while (want >= 2.0f * kPi) want -= 2.0f * kPi;
    PutF(d, 0x34, turn);
    PutF(d, 0x38, want);
}

}  // namespace

uint32_t DesireGait(const MoveDesire& m) {
    for (int i = 0; i < m.count; ++i)
        if (m.idx[i] == 0x3f) return m.val[i] & 0xff;
    return 0;
}

void LoadPuppetControlConfig(const wchar_t* ini) {
    g_corrections = GetPrivateProfileIntW(L"net", L"puppet_nudge", 1, ini) != 0;
    wchar_t w[32];
    GetPrivateProfileStringW(L"net", L"puppet_correct_m", L"0.6", w, 32, ini);
    g_correctMoving = std::clamp(static_cast<float>(_wtof(w)), 0.1f, kTeleport);
}

PuppetDriveResult DrivePuppet(uint32_t goh, PuppetDrive& st, const PuppetTarget& t, double nowMs) {
    PuppetDriveResult r;
    const float dt = st.lastMs > 0 ? std::clamp(static_cast<float>(nowMs - st.lastMs) / 1000.0f, 0.001f, 0.1f) : 1.0f / 60.0f;
    st.lastMs = nowMs;

    const uintptr_t actor = PreparePuppetLocomotion(goh);
    const bool havePose = GetPuppetPose(goh, r.cur, r.curHeading);
    if (!actor || !havePose) {
        SetPuppetTransform(goh, t.pos, t.heading);
        r.teleported = true;
        return r;
    }

    // The puppet replays the remote's input, so it follows the remote's path delayed by the network (the interpolated
    // position, leadMs old). Gaps are measured against that path: predicting the present instead would make the puppet
    // permanently "behind" by speed x delay, and pulling it forward would slide its feet.
    const Vec3 now = t.pos;
    const float ex = now.x - r.cur.x, ez = now.z - r.cur.z;
    r.err = std::sqrt(ex * ex + ez * ez);
    r.dy = t.pos.y - r.cur.y;
    const float speed = std::sqrt(t.vel.x * t.vel.x + t.vel.z * t.vel.z);
    uint8_t desire[kDesireSize];
    Decode(t.move, desire);
    // The remote's stick: the puppet copies it (pressed or released, and how far), so it starts, runs and stops with
    // the remote's own animations. Moving = the stick is pressed or the remote still moves (stopping, pushed).
    const float remoteMag = std::sqrt(GetF(desire, 0x14) * GetF(desire, 0x14) + GetF(desire, 0x16) * GetF(desire, 0x16));
    const bool stick = remoteMag > 0.05f;
    if (!stick || speed > 0.5f) st.stuckStickSince = 0;
    else if (!st.stuckStickSince) st.stuckStickSince = nowMs;
    // Stick pressed but not getting anywhere for 0.5 s: the remote walks into a wall. Treat it as standing (the puppet
    // would otherwise walk off where the remote is blocked).
    const bool blocked = st.stuckStickSince && nowMs - st.stuckStickSince > 500.0;
    r.moving = (stick && !blocked) || speed > 0.3f;
    const float hp = r.curHeading * kDeg;
    auto ahead = [hp](float ax, float az) { return ax * -std::sin(hp) + az * -std::cos(hp); };  // along our facing

    if (r.moving) {
        st.standingSince = 0;
        st.headingOffSince = 0;
        st.walkIn = false;
        const float mag = std::min(remoteMag, 1.0f);
        if (!stick) {
            Stand(desire);  // stick released: the engine's own stop, like the remote's
        } else if (speed < 0.5f) {
            // Starting: no path yet, so head where the remote's input points (its desired world heading).
            const float hd = GetF(desire, 0x38) - kPi;
            Steer(desire, r.curHeading, -std::sin(hd), -std::cos(hd), mag);
        } else {
            // Pure pursuit on the remote's path: aim 0.35 s ahead so curves are followed, not cut, with the remote's own
            // stick magnitude and gait (its walk / run / sprint animation and speed).
            const float aimX = now.x + t.vel.x * 0.35f - r.cur.x, aimZ = now.z + t.vel.z * 0.35f - r.cur.z;
            const float along = (ex * t.vel.x + ez * t.vel.z) / speed;  // + = the puppet is behind
            float m = mag;
            if (along < -0.2f) m *= std::clamp(1.0f + along, 0.4f, 0.8f);  // ahead: ease off
            if (ahead(aimX, aimZ) < 0.0f && r.err < 1.0f) {
                Stand(desire);  // the aim point is behind us: wait instead of turning around
            } else {
                if ((DesireGait(t.move) & 0x0f) == 0) SetGait(desire, speed > 3.5f ? 4u : 1u);
                Steer(desire, r.curHeading, aimX, aimZ, m);
            }
        }
    } else {
        // Remote standing: walk into the spot if it's ahead and 0.3-1 m away, else turn in place to its facing. What
        // the walk can't do precisely is left to the soft pull once the puppet stands.
        if (!st.standingSince) st.standingSince = nowMs;
        const bool spotAhead = ahead(ex, ez) > r.err * 0.5f;
        if (r.err > 0.3f && r.err < 1.0f && spotAhead) st.walkIn = true;
        if (r.err < 0.15f || !spotAhead || r.err >= 1.0f) st.walkIn = false;
        if (st.walkIn) {
            SetGait(desire, 1u);
            Steer(desire, r.curHeading, ex, ez, std::clamp(r.err * 1.5f, 0.25f, 0.7f));
        } else {
            Stand(desire);
            Face(desire, r.curHeading, t.heading);
        }
    }
    SetPuppetDesire(actor, desire);

    // On another level than the remote (it stepped onto a crate / ledge, or the puppet fell off one) for 0.2 s: the
    // puppet can't climb, so the pull includes height.
    const bool otherLevel = std::fabs(r.dy) > kLevel;
    if (!otherLevel) st.heightOffSince = 0;
    else if (!st.heightOffSince) st.heightOffSince = nowMs;
    const bool followHeight = otherLevel && nowMs - st.heightOffSince > 200.0;

    // Corrections. Teleport: a large gap, or a pull that does not close the gap (blocked by an NPC-only obstacle).
    bool teleport = r.err > kTeleport;
    if (st.correctingSince && nowMs - st.correctingSince > 600.0 && r.err > st.correctStartErr * 0.8f) teleport = true;
    // Standing still: once the puppet has stood for 0.3 s, a facing that is still > 20 deg off after 1.5 s is snapped.
    const bool settled = !r.moving && st.standingSince && nowMs - st.standingSince > 300.0 && !st.walkIn;
    float turnErr = std::fabs(WrapPi((t.heading - r.curHeading) * kDeg));
    if (!r.moving && turnErr > 20.0f * kDeg) {
        if (!st.headingOffSince) st.headingOffSince = nowMs;
    } else {
        st.headingOffSince = 0;
    }
    if (teleport) {
        SetPuppetTransform(goh, now, t.heading);
        st = PuppetDrive{.lastMs = nowMs, .teleports = st.teleports + 1};
        r.teleported = true;
        return r;
    }
    if (!g_corrections) return r;

    // moving: the gap allowed grows with speed (a sprinting puppet trails ~0.6 m after a start, invisible at 5 m/s)
    const float start = r.moving ? std::max(g_correctMoving, 0.2f * speed) : (settled ? g_correctStanding : 1.0f);
    if (!st.correctingSince && (r.err > start || followHeight)) {
        st.correctingSince = nowMs;
        st.correctStartErr = r.err;
    }
    const bool faceSnap = st.headingOffSince && nowMs - st.headingOffSince > 1500.0;
    if (st.correctingSince || faceSnap) {
        const float k = 1.0f - std::exp(-dt / kCorrectTau);
        // Pull toward the present position plus this frame's motion along the remote's velocity (the pull replaces the
        // animation's own motion for the frame).
        const Vec3 np{r.cur.x + t.vel.x * dt + (now.x - r.cur.x) * k, followHeight ? r.cur.y + r.dy * k : r.cur.y,
                      r.cur.z + t.vel.z * dt + (now.z - r.cur.z) * k};
        const float nh = r.curHeading + WrapPi((t.heading - r.curHeading) * kDeg) / kDeg * (r.moving || faceSnap ? k : 0.0f);
        r.corrected = NudgePuppet(goh, np, nh);
        if (r.err < (r.moving ? kCorrectDoneMoving : kCorrectDoneStanding) && !otherLevel) st.correctingSince = 0;
        if (faceSnap && turnErr < 3.0f * kDeg) st.headingOffSince = 0;
    }
    return r;
}

}  // namespace rdmp
