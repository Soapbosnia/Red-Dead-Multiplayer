#pragma once
// Drives one remote player's puppet from its network state (main thread, once per frame).
//
// The remote player's own movement input (the engine's locomotion "desire": stick direction, turn, gait) drives the
// puppet, so the engine plays the same walk / run / sprint animations and the animation itself moves the puppet at the
// remote's speed. The controller only steers that input along the remote's path and corrects drift; it moves the puppet
// directly (a soft pull over a few frames) only when the gap gets large, and teleports only when that fails.
// Measured: moving the puppet every frame cancels the animation's own motion (the feet slide), so corrections are rare.
#include <cstdint>

#include "actors.h"
#include "shared/types.h"

namespace rdmp {

struct PuppetTarget {
    Vec3 pos;              // interpolated network position (interp_ms behind the newest sample)
    Vec3 vel;              // remote velocity (m/s, horizontal) around pos
    float heading = 0.0f;  // remote facing (deg)
    float leadMs = 0.0f;   // how old pos is (the puppet shows the remote this much late)
    net::MoveInput input;  // the remote's movement input
    // The game's player code moves the puppet from the remote's controls (puppet_player.h): no movement request of
    // ours, no AI brain; only the position corrections below.
    bool playerDriven = false;
};

// Per-puppet controller state.
struct PuppetDrive {
    double standingSince = 0;     // since when the remote stands (0 = moving)
    double headingOffSince = 0;   // standing: since when the puppet faces > 20 deg away (0 = not)
    double heightOffSince = 0;    // since when the puppet is on another level than the remote (0 = not)
    double correctingSince = 0;   // soft pull active since (0 = not)
    double stuckStickSince = 0;   // remote stick pressed while (almost) not moving, since (0 = not)
    double lastMs = 0;            // last frame time
    float correctStartErr = 0;    // gap when the pull started
    bool walkIn = false;          // standing: walking into the exact spot
    int teleports = 0;
};

struct PuppetDriveResult {
    Vec3 cur{};              // puppet position before this frame's correction
    float curHeading = 0.0f;
    float err = -1.0f;       // horizontal gap to the remote's (delayed) network position (m), -1 = no pose
    float dy = 0.0f;         // remote height - puppet height
    bool moving = false;
    bool corrected = false;  // the pull moved the puppet this frame
    bool teleported = false;
};

// rdmp.ini [net] puppet_nudge (1 = soft corrections on, default), puppet_correct_m (gap that starts a correction while
// moving, default 0.6).
void LoadPuppetControlConfig(const wchar_t* ini);

PuppetDriveResult DrivePuppet(uint32_t goh, PuppetDrive& st, const PuppetTarget& t, double nowMs);

}  // namespace rdmp
