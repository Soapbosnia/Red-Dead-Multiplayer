// Server-driven radar/pause-map blips (see blips.h). Engine side: the game's own blip natives, called through the
// native registry (scriptnatives.h). RE notes (docs/SCRIPTING_RE.md "Blips"):
//   - ADD_BLIP_FOR_COORD (0x140653B90) -> FUN_1404C5360(loader, &out, icon, &vec, ...): blip pool at loader+0x2270,
//     0xC7 (199) slots shared with the game's own blips; returns 0 when full.
//   - every blip native first checks the HUD entry list (loader+0x25760, current index +0x18): blips only work once
//     the HUD is up, i.e. after world load (the mod's BootStep HUD-on step).
//   - icons are BlipIcon ids 299..572 (FUN_140606C60 special-cases 299..566); the native's float arg is the min-distance (pass 0).
//   - SET_BLIP_COLOR takes r,g,b,a as 0..1 floats; SET_BLIP_MAX_DISTANCE < 0 = unlimited.
#include "blips.h"

#include <cmath>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "shared/fw_api.h"
#include "log.h"
#include "scriptnatives.h"

namespace rdmp {
namespace {

constexpr int kMaxLive = 120;            // leave the rest of the 199-slot pool to the game
constexpr double kPosEpsilon = 0.05;     // m: smaller moves are not re-sent to the engine
constexpr uint16_t kIconMin = 299, kIconMax = 572;  // BlipIcon range (native DB)

struct Live {
    int32_t handle = 0;
    WantedBlip applied{};
    Vec3 lastPos{};
    bool colorApplied = false;
};

std::unordered_map<uint64_t, Live> g_live;
std::unordered_set<uint64_t> g_failed;  // ids whose creation failed (logged once; retried every ~2 s)
uint32_t g_frame = 0;
bool g_loggedFull = false;

bool Valid(int32_t handle) {
    if (!handle) return false;
    NativeCall c;
    c.Int(handle);
    return c.Call(nat::IS_BLIP_VALID) && c.ResultBool();
}

void Remove(int32_t handle) {
    if (!handle) return;
    NativeCall c;
    c.Int(handle);
    c.Call(nat::REMOVE_BLIP);
}

uint16_t SafeIcon(uint16_t icon) {
    return (icon >= kIconMin && icon <= kIconMax) ? icon : static_cast<uint16_t>(330);  // BLIP_OBJECTIVE
}

void ApplyProps(Live& live, const WantedBlip& w, bool fresh) {
    const WantedBlip& a = live.applied;
    if (!fresh && SafeIcon(w.icon) != SafeIcon(a.icon)) {
        NativeCall c;
        c.Int(live.handle).Int(SafeIcon(w.icon));
        c.Call(nat::CHANGE_BLIP_ICON);
    }
    // Colour only once a script set one (flag kHasColor): the icons carry their own colours, and a tint of
    // white is not guaranteed to be the engine's neutral default.
    const bool wantColor = (w.flags & fw::kBlipHasColor) != 0;
    if (wantColor && (fresh || !live.colorApplied || w.rgba != a.rgba)) {
        NativeCall c;
        c.Int(live.handle)
            .Float(static_cast<float>((w.rgba >> 24) & 0xff) / 255.0f)
            .Float(static_cast<float>((w.rgba >> 16) & 0xff) / 255.0f)
            .Float(static_cast<float>((w.rgba >> 8) & 0xff) / 255.0f)
            .Float(static_cast<float>(w.rgba & 0xff) / 255.0f);
        c.Call(nat::SET_BLIP_COLOR);
        live.colorApplied = true;
    }
    if ((fresh && w.scale != 1.0f) || (!fresh && w.scale != a.scale)) {
        NativeCall c;
        c.Int(live.handle).Float(w.scale);
        c.Call(nat::SET_BLIP_SCALE);
    }
    if ((fresh && w.maxDistance >= 0.0f) || (!fresh && w.maxDistance != a.maxDistance)) {
        NativeCall c;
        c.Int(live.handle).Float(w.maxDistance);
        c.Call(nat::SET_BLIP_MAX_DISTANCE);
    }
    if ((fresh && w.priority != 0) || (!fresh && w.priority != a.priority)) {
        NativeCall c;
        c.Int(live.handle).Int(w.priority);
        c.Call(nat::SET_BLIP_PRIORITY);
    }
    const uint8_t changed = fresh ? 0xff : static_cast<uint8_t>(w.flags ^ a.flags);
    if (changed & fw::kBlipHidden) {
        NativeCall c;
        c.Int(live.handle).Bool((w.flags & fw::kBlipHidden) == 0);
        if (!fresh || (w.flags & fw::kBlipHidden)) c.Call(nat::SET_BLIP_VISIBLE);
    }
    if ((changed & fw::kBlipRadarOnly) && (!fresh || (w.flags & fw::kBlipRadarOnly))) {
        NativeCall c;
        c.Int(live.handle).Bool((w.flags & fw::kBlipRadarOnly) != 0);
        c.Call(nat::SET_BLIP_HUDMAP_ONLY);
    }
    if ((changed & fw::kBlipMapOnly) && (!fresh || (w.flags & fw::kBlipMapOnly))) {
        NativeCall c;
        c.Int(live.handle).Bool((w.flags & fw::kBlipMapOnly) != 0);
        c.Call(nat::SET_BLIP_PAUSEMAP_ONLY);
    }
    live.applied = w;
}

int32_t Create(const WantedBlip& w, const Vec3& pos) {
    float v[3] = {pos.x, pos.y, pos.z};
    NativeCall c;
    c.Ptr(v).Int(SafeIcon(w.icon)).Float(0.0f).Bool(false).Int(0);
    if (!c.Call(nat::ADD_BLIP_FOR_COORD)) return 0;
    return c.ResultInt();
}

}  // namespace

void BlipsForget() {
    g_live.clear();
    g_failed.clear();
}

int LiveBlipCount() { return static_cast<int>(g_live.size()); }

void ApplyBlips(const WantedBlip* wanted, int count, ElementPositionFn resolve, void* user) {
    ++g_frame;
    std::unordered_set<uint64_t> seen;
    seen.reserve(static_cast<size_t>(count) * 2 + 1);

    for (int i = 0; i < count; ++i) {
        const WantedBlip& w = wanted[i];
        seen.insert(w.id);
        Vec3 pos = w.pos;
        if (w.attachedTo && resolve) {
            Vec3 live{};
            if (resolve(w.attachedTo, live, user)) pos = live;  // else: the server's copy of the target's position
        }

        auto it = g_live.find(w.id);
        if (it != g_live.end() && !Valid(it->second.handle)) {
            Log("blips: engine dropped blip %llu (handle %08x), re-creating", static_cast<unsigned long long>(w.id),
                static_cast<uint32_t>(it->second.handle));
            g_live.erase(it);
            it = g_live.end();
        }
        if (it == g_live.end()) {
            if (static_cast<int>(g_live.size()) >= kMaxLive) {
                if (!g_loggedFull) {
                    g_loggedFull = true;
                    Log("blips: %d blips is the limit for script blips (engine pool is 199, shared with the game)", kMaxLive);
                }
                continue;
            }
            const bool failedBefore = g_failed.count(w.id) != 0;
            if (failedBefore && (g_frame % 120) != 0) continue;  // retry roughly every 2 s
            const int32_t handle = Create(w, pos);
            if (!handle) {
                if (!failedBefore) {
                    g_failed.insert(w.id);
                    Log("blips: ADD_BLIP_FOR_COORD failed for blip %llu (icon %u) - HUD not ready or pool full",
                        static_cast<unsigned long long>(w.id), w.icon);
                }
                continue;
            }
            g_failed.erase(w.id);
            Live live;
            live.handle = handle;
            live.lastPos = pos;
            ApplyProps(live, w, true);
            g_live.emplace(w.id, live);
            Log("blips: created %llu -> handle %08x icon %u at (%.1f, %.1f, %.1f)%s", static_cast<unsigned long long>(w.id),
                static_cast<uint32_t>(handle), SafeIcon(w.icon), pos.x, pos.y, pos.z, w.attachedTo ? " (attached)" : "");
            continue;
        }

        Live& live = it->second;
        ApplyProps(live, w, false);
        const double dx = pos.x - live.lastPos.x, dy = pos.y - live.lastPos.y, dz = pos.z - live.lastPos.z;
        if (dx * dx + dy * dy + dz * dz > kPosEpsilon * kPosEpsilon) {
            float v[3] = {pos.x, pos.y, pos.z};
            NativeCall c;
            c.Int(live.handle).Ptr(v);
            c.Call(nat::SET_BLIP_POS);
            live.lastPos = pos;
        }
    }

    for (auto it = g_live.begin(); it != g_live.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        Remove(it->second.handle);
        Log("blips: removed %llu (handle %08x)", static_cast<unsigned long long>(it->first),
            static_cast<uint32_t>(it->second.handle));
        it = g_live.erase(it);
    }
    for (auto it = g_failed.begin(); it != g_failed.end();) {
        it = seen.count(*it) ? std::next(it) : g_failed.erase(it);
    }
    if (count == 0) g_loggedFull = false;
}

}  // namespace rdmp
