// Host table for client script natives (shared/fw_api.h). Called by rdmp_client.dll on the game's main thread.
#include "host.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "log.h"
#include "sandbox.h"
#include "scriptnatives.h"

namespace rdmp {
namespace {

void HostLog(const char* line) { Log("%s", line); }

bool HostInvokeNative(uint32_t hash, const uint64_t* args, int argc, uint64_t* result) {
    NativeCall call;
    for (int i = 0; i < argc; ++i) call.Uint64(args[i]);
    if (!call.Call(hash)) return false;
    if (result) *result = call.ResultRaw();
    return true;
}

bool HostGetLocalPlayer(float pos[3], float* heading, float* health, uint32_t* actorHandle) {
    Vec3 p{};
    float h = 0.0f;
    if (!GetLocalPlayerState(p, h)) return false;
    if (pos) {
        pos[0] = p.x;
        pos[1] = p.y;
        pos[2] = p.z;
    }
    if (heading) *heading = h;
    if (health) {
        float hp = 0.0f;
        *health = GetLocalPlayerHealth(hp) ? hp : 0.0f;
    }
    if (actorHandle) {
        // The script-level Actor handle (what natives take), not the internal one cached at spawn
        // (playerInfo+0x5ec, e.g. 2 where GET_PLAYER_ACTOR gives 529).
        NativeCall c;
        c.Int(0);
        *actorHandle = c.Call(NativeHash("GET_PLAYER_ACTOR")) ? c.ResultUint() : 0;
    }
    return true;
}

void HostTeleport(const float pos[3], float heading) { TeleportLocalPlayer(Vec3{pos[0], pos[1], pos[2]}, heading); }
void HostFade(int fadeIn, float ms) { ScriptFadeCamera(fadeIn != 0, ms); }
void HostSetControl(int enabled) { ScriptSetPlayerControl(enabled != 0); }
void HostCamera() { ScriptSetCameraTarget(); }
void HostSetHealth(float hp) { ScriptSetPlayerHealth(hp); }

int HostFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

// On-screen line: PRINT_OBJECTIVE_B(text, seconds, literal, int, bool, const char*, bool, const char*), the subtitle
// line at the bottom centre. (Tested in game: PRINT_SMALL_B is never accepted by the HUD in the sandbox, and the
// PRINT_HELP_B box is accepted but not drawn.) One line at a time: the HUD drops a new objective while one is shown,
// and CLEAR_PRINTED_OBJECTIVE in the same frame also wipes the new one. So a line that arrives while another is up
// clears it now and is printed a little later (HostFrame, kLineGapMs: the cleared line takes a moment to go, and
// prints before that are dropped too); the newest line wins.
constexpr ULONGLONG kLineGapMs = 400;
char g_lineText[256];
float g_lineSeconds = 0.0f;
bool g_linePending  = false;
ULONGLONG g_lineAt    = 0;
ULONGLONG g_lineUntil = 0;

void PrintNow() {
    NativeCall c;
    c.Ptr(g_lineText).Float(g_lineSeconds).Bool(true).Int(0).Bool(false).Ptr(nullptr).Bool(false).Ptr(nullptr);
    c.Call(NativeHash("PRINT_OBJECTIVE_B"));
    g_lineUntil   = GetTickCount64() + static_cast<ULONGLONG>(g_lineSeconds * 1000.0f);
    g_linePending = false;
}

void HostPrint(const char* text, int ms) {
    // The engine may keep the pointer while the line is up, so the text lives in a static buffer that is only
    // rewritten after a clear.
    strncpy_s(g_lineText, text ? text : "", _TRUNCATE);
    g_lineSeconds = static_cast<float>(ms > 0 ? ms : 4000) / 1000.0f;
    Log("print: %s", g_lineText);
    if (g_linePending) return;  // a clear is already under way; the newest text goes out when it is done
    if (GetTickCount64() < g_lineUntil) {
        NativeCall clear;
        clear.Call(NativeHash("CLEAR_PRINTED_OBJECTIVE"));
        g_lineUntil   = 0;
        g_linePending = true;
        g_lineAt      = GetTickCount64() + kLineGapMs;
        return;
    }
    PrintNow();
}

const fw::Host g_host = {
    fw::kApiVersion, &HostLog,     &HostInvokeNative, &HostGetLocalPlayer, &HostTeleport, &HostFade,
    &HostSetControl, &HostCamera,  &HostSetHealth,    &HostFocus,          &HostPrint,
};

}  // namespace

const fw::Host* GameHost() { return &g_host; }

void HostFrame() {
    if (g_linePending && GetTickCount64() >= g_lineAt) PrintNow();
}

}  // namespace rdmp
