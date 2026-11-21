// Boot splash removal using the game's own boot flow (no covering windows, no skipped logic).
//
// The boot screen is drawn by FUN_1405f4390 (render thread). It clears the back buffer to black (FUN_14012df00 with
// 0xff000000), then, while the legals controller *(0x142AC5130) exists and its countdown (+8) is > 1000, calls
// FUN_1405f4cc0 to draw the legal pages (a Flash movie) and the Rockstar logo Bink video on top.
//
// skip_legals=1: that one draw call (0x1405F44F2) is NOPed. The legals logic itself still runs unchanged (the movie
//   advances, posts "EndLegals", the platform sign-in finishes behind it as usual) - only nothing is drawn, so the
//   screen stays engine black. Skipping the controller's logic instead (countdown=4 + done flags) outruns the platform
//   sign-in and the game shows "You have been logged out of Rockstar Games".
// skip_videos=1: every Bink video ends the moment it opens (FrameNum = Frames), which runs the normal "video finished"
//   path. Returning null from BinkOpen instead stalls the boot forever (the controller waits for the player's
//   playing flag +0x79 to clear, which only happens on a real handle).
#include "bootvideo.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kDrawLegalsCall = 0x1405F44F2;  // CALL FUN_1405f4cc0 in FUN_1405f4390 (e8 c9 07 00 00)

// The loader's "loading" Scaleform movie wrapper (FUN_140631f50, vtable 0x141CE4118 - used only by the loader) draws
// the LoadingScreen scene (red brush art + revolver spinner) in vtable slot +0x28 (FUN_1401157d0 -> movie->Display).
// Advance (+0x18) is separate, so with the draw stubbed the scene still runs its ActionScript (Loading* events,
// transitions, loadComplete) but nothing is drawn over the engine's black clear.
constexpr uintptr_t kLoadingMovieVtDraw = 0x141CE4118 + 0x28;
constexpr uintptr_t kLoadingMovieDrawOrig = 0x1401157D0;
constexpr uintptr_t kTitleMovieVtDraw = 0x141CE5318 + 0x28;  // "startscreen" wrapper (title logo)
// (The loader's fader movie, loader+0xc418, is a different class - vtable 0x141CE3C18 - and is not affected.)
// Note: while the world loads, the loading render list draws only these wrappers, so with them hidden the screen shows
// the engine's empty-world output (very dark grey, ~#1C1819), not pure black. The fader is not in that list (drawing
// it from here has no visible effect) - see NOTES.md "Boot splash / loading screen".
void LoadingMovieNoDraw(void* self) {
    static void* seen[8] = {};
    for (auto& s : seen) {
        if (s == self) break;
        if (!s) {
            s = self;
            Log("loading screen: movie wrapper %p hidden", self);
            break;
        }
    }
}
// Debug ([debug] ui_draw_log=1): logs each distinct UI movie wrapper drawn through FUN_1401157d0 (name at +0x68).
constexpr uintptr_t kUiWrapperDraw = 0x1401157D0;
constexpr uint8_t kUiWrapperDrawPrologue[12] = {0x40, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x05, 0x00, 0x00, 0x48, 0x8B, 0xF9};
void (*g_uiDrawOrig)(void*) = nullptr;
void UiWrapperDrawHook(void* self) {
    static void* seen[64] = {};
    for (auto& s : seen) {
        if (s == self) break;
        if (!s) {
            s = self;
            const auto* p = static_cast<const uint8_t*>(self);
            Log("ui draw: wrapper %p vt=%p name=\"%.31s\" visible(+0x19)=%d movie=%p hidden(+0x41)=%d", self,
                *reinterpret_cast<void* const*>(p), reinterpret_cast<const char*>(p + 0x68), p[0x19],
                *reinterpret_cast<void* const*>(p + 0x48), p[0x41]);
            break;
        }
    }
    {  // periodic state of the fader wrapper
        const auto* p = static_cast<const uint8_t*>(self);
        static ULONGLONG lastFader = 0;
        if (std::strncmp(reinterpret_cast<const char*>(p + 0x68), "fader", 6) == 0 && GetTickCount64() - lastFader > 1000) {
            lastFader = GetTickCount64();
            Log("ui draw: fader visible=%d movie=%p hidden=%d +0x40=%d", p[0x19], *reinterpret_cast<void* const*>(p + 0x48), p[0x41], p[0x40]);
        }
    }
    g_uiDrawOrig(self);
}
using BinkOpenFn = void* (__stdcall*)(const char* name, unsigned flags);
BinkOpenFn g_realBinkOpen = nullptr;
bool g_skipVideos = false;

void* __stdcall BinkOpenHook(const char* name, unsigned flags) {
    void* bink = g_realBinkOpen(name, flags);
    // Bink header: +8 Frames, +0xc FrameNum. The player (FUN_140752740) stops as soon as FrameNum >= Frames.
    if (bink && g_skipVideos) {
        auto* w = reinterpret_cast<uint32_t*>(bink);
        w[3] = w[2];
    }
    Log("video: BinkOpen(%s, 0x%x) -> %p%s", (flags & 0x4000000) ? "<memory>" : (name ? name : "(null)"), flags, bink,
        bink && g_skipVideos ? " (ended immediately)" : "");
    return bink;
}


}  // namespace

void InstallBootVideoHook(const wchar_t* ini) {
    g_skipVideos = GetPrivateProfileIntW(L"sandbox", L"skip_videos", 0, ini) != 0;
    g_realBinkOpen = reinterpret_cast<BinkOpenFn>(PatchImport("BINK2W64.DLL", "BinkOpen", reinterpret_cast<void*>(&BinkOpenHook)));
    Log("video: BinkOpen hook %s (skip_videos=%d)", g_realBinkOpen ? "installed" : "NOT installed (import not found)", g_skipVideos);
    if (GetPrivateProfileIntW(L"sandbox", L"skip_legals", 0, ini)) {
        static const uint8_t kExpect[] = {0xE8, 0xC9, 0x07, 0x00, 0x00};
        static const uint8_t kNop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
        const bool match = std::memcmp(reinterpret_cast<void*>(GameAddr(kDrawLegalsCall)), kExpect, sizeof(kExpect)) == 0;
        const bool ok = match && WriteCode(GameAddr(kDrawLegalsCall), kNop5, sizeof(kNop5));
        Log("legals: draw call removed: %s", ok ? "ok" : (match ? "WRITE FAILED" : "bytes mismatch"));
    }
    if (GetPrivateProfileIntW(L"sandbox", L"hide_loading_screen", 0, ini)) {
        auto* slot = reinterpret_cast<uintptr_t*>(GameAddr(kLoadingMovieVtDraw));
        const bool match = *slot == GameAddr(kLoadingMovieDrawOrig);
        const uintptr_t stub = reinterpret_cast<uintptr_t>(&LoadingMovieNoDraw);
        const bool ok = match && WriteCode(reinterpret_cast<uintptr_t>(slot), &stub, sizeof(stub));
        Log("loading screen: draw removed: %s", ok ? "ok" : (match ? "WRITE FAILED" : "vtable mismatch"));
    }
    if (GetPrivateProfileIntW(L"sandbox", L"hide_title_screen", 0, ini)) {
        // The title screen ("startscreen" movie wrapper, vtable 0x141CE5318, created only by the loader at loader+0xc4a0)
        // flashes the RED DEAD REDEMPTION logo for a moment between setupComplete and the direct start.
        auto* slot = reinterpret_cast<uintptr_t*>(GameAddr(kTitleMovieVtDraw));
        const bool match = *slot == GameAddr(kLoadingMovieDrawOrig);
        const uintptr_t stub = reinterpret_cast<uintptr_t>(&LoadingMovieNoDraw);
        const bool ok = match && WriteCode(reinterpret_cast<uintptr_t>(slot), &stub, sizeof(stub));
        Log("title screen: draw removed: %s", ok ? "ok" : (match ? "WRITE FAILED" : "vtable mismatch"));
    }
    if (GetPrivateProfileIntW(L"debug", L"ui_draw_log", 0, ini)) {
        g_uiDrawOrig = reinterpret_cast<void (*)(void*)>(
            DetourPic(GameAddr(kUiWrapperDraw), kUiWrapperDrawPrologue, sizeof(kUiWrapperDrawPrologue), reinterpret_cast<void*>(&UiWrapperDrawHook)));
        Log("ui draw log: %s", g_uiDrawOrig ? "on" : "detour FAILED");
    }
}

}  // namespace rdmp
