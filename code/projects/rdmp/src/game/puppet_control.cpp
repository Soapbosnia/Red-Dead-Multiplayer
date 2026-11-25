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

using net::MoveInput;

constexpr float kPi = 3.14159265f;
constexpr float kDeg = 0.01745329f;

bool g_corrections = true;
float g_correctMoving = 0.6f;   // gap (m) that starts a soft pull while the remote moves
float g_correctStanding = 0.3f;  // ... and once the puppet has stood still for a while (smaller gaps stay)
constexpr float kCorrectDoneMoving = 0.25f;   // the pull ends below this gap (moving: the animation lags by the delay)
constexpr float kCorrectDoneStanding = 0.04f;
constexpr float kCorrectTau = 0.12f;          // moving pull time constant (s): ~90% of the gap in a quarter second
constexpr float kCorrectTauStanding = 0.6f;   // standing: a slow drift into place (no animation to fight, nothing snaps)
constexpr double kSettleMs = 800.0;           // standing this long (the stop animation is over) before any correction
constexpr float kTeleport = 4.0f;      // larger gaps are teleported
bool g_mtaCorrection = true;  // [net] puppet_correction=mta (default) | classic
float g_mtaMin = 0.05f;        // [net] puppet_correct_min_m: smaller gaps are not corrected (mta)
float g_level = 0.5f;  // a height difference above this = the remote is on another level (crate, step); [net] puppet_level_m

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

// Rebuilds a desire from the remote's input: the words a player on foot produces (shared/types.h); [0x34] (turn) is
// recomputed against the puppet's own facing by Steer / Face.
void Decode(const MoveInput& in, uint8_t* desire) {
    std::memset(desire, 0, kDesireSize);
    const bool stick = in.right != 0.0f || in.back != 0.0f;
    PutU(desire, 0x35, 0xbf800000u);  // -1
    PutU(desire, 0x3a, 0xbf800000u);
    PutU(desire, 0x3f, 0xc2280000u | in.gait);
    PutU(desire, 0x40, 0x1000u);
    if (stick) {
        PutF(desire, 0x14, in.right);
        PutU(desire, 0x15, 0x80000000u);
        PutF(desire, 0x16, in.back);
        PutF(desire, 0x38, in.heading);
    }
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

MoveInput MoveInputFromDesire(const uint8_t* desire) {
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

void LoadPuppetControlConfig(const wchar_t* ini) {
    g_corrections = GetPrivateProfileIntW(L"net", L"puppet_nudge", 1, ini) != 0;
    {
        wchar_t mode[32];
        GetPrivateProfileStringW(L"net", L"puppet_correction", L"mta", mode, 32, ini);
        g_mtaCorrection = _wcsicmp(mode, L"classic") != 0;
        GetPrivateProfileStringW(L"net", L"puppet_correct_min_m", L"0.05", mode, 32, ini);
        g_mtaMin = std::clamp(static_cast<float>(_wtof(mode)), 0.0f, 2.0f);
    }
    {
        wchar_t lv[32];
        GetPrivateProfileStringW(L"net", L"puppet_level_m", L"0.5", lv, 32, ini);
        g_level = std::clamp(static_cast<float>(_wtof(lv)), 0.02f, 4.0f);
    }
    wchar_t w[32];
    GetPrivateProfileStringW(L"net", L"puppet_correct_m", L"0.6", w, 32, ini);
    g_correctMoving = std::clamp(static_cast<float>(_wtof(w)), 0.1f, kTeleport);
}

PuppetDriveResult DrivePuppet(uint32_t goh, PuppetDrive& st, const PuppetTarget& t, double nowMs) {
    PuppetDriveResult r;
    const float dt = st.lastMs > 0 ? std::clamp(static_cast<float>(nowMs - st.lastMs) / 1000.0f, 0.001f, 0.1f) : 1.0f / 60.0f;
    st.lastMs = nowMs;

    const uintptr_t actor = t.playerDriven ? ActorFromGohHandle(goh) : PreparePuppetLocomotion(goh);
    const bool havePose = GetPuppetPose(goh, r.cur, r.curHeading);
    if (!actor || !havePose) {
        SetPuppetTransform(goh, t.pos, t.heading);
        r.teleported = true;
        return r;
    }

    // MTA's correction (CClientPed::SetTargetPosition / UpdateTargetPosition, docs/MTA_COMPARISON.md): the target is the
    // newest received state, not an interpolated past one; every frame closes a share of the remaining error (all three
    // axes) on top of the puppet's own movement from the remote's controls, so it always converges (about 63% in
    // kMtaTau); the facing is the remote's; a gap past a speed-scaled threshold is warped.
    if (g_mtaCorrection && t.playerDriven && t.haveNewest) {
        constexpr float kMtaTau = 0.1f;
        const Vec3& tgt = t.newestPos;
        const float ex = tgt.x - r.cur.x, ey = tgt.y - r.cur.y, ez = tgt.z - r.cur.z;
        r.err = std::sqrt(ex * ex + ez * ez);
        r.dy = ey;
        const float speed = std::sqrt(t.vel.x * t.vel.x + t.vel.z * t.vel.z);
        r.moving = speed > 0.3f;
        const float warp = 3.0f + 0.5f * speed;
        if (std::sqrt(ex * ex + ey * ey + ez * ez) > warp) {
            SetPuppetTransform(goh, tgt, t.newestHeading);
            r.teleported = true;
            return r;
        }
        if (!g_corrections) return r;
        // Below g_mtaMin the puppet's own movement is left alone: every transform message resets the character
        // controller's ground state (the game then treats the puppet as in the air for that frame).
        if (std::sqrt(ex * ex + ey * ey + ez * ez) < g_mtaMin && std::fabs(WrapPi((t.newestHeading - r.curHeading) * kDeg)) < 5.0f * kDeg)
            return r;
        const float k = 1.0f - std::exp(-dt / kMtaTau);
        // plus this frame's motion along the remote's velocity: a nudge replaces the movement the puppet's own
        // animation would have made this frame
        const Vec3 np{r.cur.x + t.vel.x * dt + ex * k, r.cur.y + ey * k, r.cur.z + t.vel.z * dt + ez * k};
        r.corrected = NudgePuppet(goh, np, t.newestHeading);
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
    Decode(t.input, desire);
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
            // Direction: the remote's own desired heading (where its stick points, so turns replay with the same
            // turning animation), bent toward its path by the sideways gap, at most ~20 deg: the gap is closed over a
            // lookahead of max(1.5 m, 0.5 s) instead of by steering hard at a near point (that made the puppet weave).
            const float hd = GetF(desire, 0x38) - kPi;
            const float dx = -std::sin(hd), dz = -std::cos(hd);
            const float look = std::max(1.5f, 0.5f * speed);
            const float onDir = ex * dx + ez * dz;
            float latX = ex - onDir * dx, latZ = ez - onDir * dz;
            const float lat = std::sqrt(latX * latX + latZ * latZ), latMax = look * 0.36f;
            if (lat > latMax) latX *= latMax / lat, latZ *= latMax / lat;
            const float aimX = dx * look + latX, aimZ = dz * look + latZ;
            const float along = (ex * t.vel.x + ez * t.vel.z) / speed;  // + = the puppet is behind
            float m = mag;
            if (along < -0.2f) m *= std::clamp(1.0f + along, 0.4f, 0.8f);  // ahead: ease off
            if ((t.input.gait & 0x0f) == 0) SetGait(desire, speed > 3.5f ? 4u : 1u);
            Steer(desire, r.curHeading, aimX, aimZ, m);
        }
    } else {
        // Remote standing. The puppet plays its own stop first (the stick was released, like the remote's) and is
        // left alone while it does. Then: a gap of 0.7-2 m ahead is walked, releasing the stick 0.4 m short so the
        // walk ends with the normal stop animation; smaller gaps drift in slowly (see the pull below); and the puppet
        // turns in place to the remote's facing. (A walk-in down to 15 cm ended in an abrupt stop: it looked snappy.)
        if (!st.standingSince) st.standingSince = nowMs;
        const bool stopped = nowMs - st.standingSince > kSettleMs;
        const bool spotAhead = ahead(ex, ez) > r.err * 0.5f;
        if (stopped && r.err > 0.7f && r.err < 2.0f && spotAhead) st.walkIn = true;
        if (r.err < 0.4f || !spotAhead || r.err >= 2.0f) st.walkIn = false;
        if (st.walkIn) {
            SetGait(desire, 1u);
            Steer(desire, r.curHeading, ex, ez, std::clamp(r.err, 0.4f, 0.8f));
        } else {
            Stand(desire);
            if (stopped) Face(desire, r.curHeading, t.heading);
        }
    }
    if (!t.playerDriven) SetPuppetDesire(actor, desire);

    // On another level than the remote (it stepped onto a crate / ledge, or the puppet fell off one) for 0.2 s: the
    // puppet can't climb, so the pull includes height.
    const bool otherLevel = std::fabs(r.dy) > g_level;
    if (!otherLevel) st.heightOffSince = 0;
    else if (!st.heightOffSince) st.heightOffSince = nowMs;
    const bool followHeight = otherLevel && nowMs - st.heightOffSince > 200.0;

    // Corrections. Teleport: a large gap, or a pull that does not close the gap (blocked by an NPC-only obstacle).
    bool teleport = r.err > kTeleport;
    if (st.correctingSince && nowMs - st.correctingSince > 600.0 && r.err > st.correctStartErr * 0.8f) teleport = true;
    // Standing still: a facing still > 20 deg off after 1.5 s (the turn in place did not take) is eased in.
    const bool settled = !r.moving && st.standingSince && nowMs - st.standingSince > kSettleMs && !st.walkIn;
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
    // Player-driven puppets lag the remote by the network delay (~0.5 m walking) and every pull cancels the
    // animation's own motion for the frame, so they get more room.
    const float moveStart = t.playerDriven ? std::max(1.0f, 0.25f * speed) : std::max(g_correctMoving, 0.2f * speed);
    const float start = r.moving ? moveStart : (settled ? g_correctStanding : 1.0f);
    if (!st.correctingSince && (r.err > start || followHeight)) {
        st.correctingSince = nowMs;
        st.correctStartErr = r.err;
    }
    const bool faceSnap = st.headingOffSince && nowMs - st.headingOffSince > 1500.0;
    if (st.correctingSince || faceSnap) {
        const float k = 1.0f - std::exp(-dt / (r.moving ? kCorrectTau : kCorrectTauStanding));
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
