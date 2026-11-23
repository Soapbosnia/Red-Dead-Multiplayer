// Our own game flow: the game's main loop, boot sequence and loading-mode frame are replaced by ours, so the boot
// splash (legal pages, Rockstar logo Bink), the "$/content/PressStart" front-end script and the loading-screen drawing
// never execute. rdmp.ini [game] own_loop=1.
//
// How RDR.exe is structured (see NOTES.md "Main loop"):
//   main FUN_14007f860 -> app FUN_1401fd270 (vt 0x141C7B008): app+8 creates the game object (vt 0x141C7AFD0,
//   *(0x142C456D0)) and calls Game::Init (slot +8, FUN_1401ee680); app+0x18 tail-jumps to Game::Run (slot +0x10,
//   FUN_1401f3140), the frame loop. Game::Update is slot +0x28 (FUN_1401f5820).
//   While the render manager *(0x142AC50D0)+0x34 ("loading mode") is set, the render thread (FUN_1405d9780) draws
//   frames with FUN_1405f4390 - the boot/loading frame: legals controller, Bink loading movies, loading UI.
//
// What we install:
//   * OurRun     -> game vtable slot +0x10. Same per-frame engine calls as FUN_1401f3140, minus the PressStart
//                   script, plus our boot driver (BootStep).
//   * Init       -> the legals/logo block of Game::Init is skipped (no controller, no logo file, no video).
//   * OurLoadingFrame replaces FUN_1405f4390: frame begin, the engine's streaming/resource bookkeeping, black clear,
//                   UI advance without drawing (HUD/fader movies load inside their Advance), present.
//   * Loading/title screen movie wrappers: Advance + Draw are no-ops (never loaded, ticked or drawn).
//   * BootStep (our boot sequence, once per frame before Game::Update):
//       0  wait for the game's own signed-in check (FUN_140ef0e70 - what the legals pages were implicitly waiting
//          ~7 s for; without it the game shows "logged out of Rockstar Games"), then open the boot gate (uiMgr+0x78)
//          that the logo video's end used to open.
//       1  BeginWorldLoad: what the front end leads to - world streaming start (normally done when the title screen
//          shows), HUD map + UI movie load requests, close the boot "Startup" scene, new-game flag.
//       2  hold the loader fader at black until the level-load trigger FUN_1401f38d0 runs the level load; drop the
//          "effects" UI phase the loading screen's cleanup would pop.
//       3  stream_ms of world streaming, then finish the load (FinishLoading -> "loadComplete" -> sandbox spawn).
//       4  HUD on (loader+0x257c0), radar map flags + radar zoom, request the "blips" UI txd (pause map).
#include "gameflow.h"

#include <intrin.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "events.h"
#include "log.h"
#include "netclient.h"
#include "uifiles.h"
#include "patch.h"
#include "sandbox.h"

namespace rdmp {
namespace {

template <typename T>
T& G(uintptr_t ghidraAddr) {
    return *reinterpret_cast<T*>(GameAddr(ghidraAddr));
}
template <typename T>
T& At(uintptr_t p, uintptr_t off) {
    return *reinterpret_cast<T*>(p + off);
}
template <typename Fn>
Fn F(uintptr_t ghidraAddr) {
    return reinterpret_cast<Fn>(GameAddr(ghidraAddr));
}
// virtual call helper: slot is a byte offset into the vtable
template <typename Fn>
Fn V(uintptr_t obj, uintptr_t slot) {
    return reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + slot));
}

// globals
constexpr uintptr_t kRenderMgr = 0x142AC50D0;   // render manager: +0x34 loading mode, +0x48/+0x50 mutexes
constexpr uintptr_t kGfxFrame = 0x142AC50C8;    // frame begin (+0x20) / end+present (+0x28)
constexpr uintptr_t kStreamer = 0x142AC4C98;    // streaming bookkeeping object (+0x1cb0 critical section)
constexpr uintptr_t kDeviceHolder = 0x142AD2130;  // +0x530 = device
constexpr uintptr_t kLoader = 0x142C456C8;
constexpr uintptr_t kUiMgr = 0x142AAC8B8;
constexpr uintptr_t kPlatform = 0x142AAC8D8;
constexpr uintptr_t kCamMgr = 0x142C45738;
constexpr uintptr_t kGameVtable = 0x141C7AFD0;
constexpr uintptr_t kRenderThreadOwner = 0x1422722E0;  // thread id that owns the device (or -1)

bool g_installed = false;
volatile bool g_quitRequested = false;  // the menu's Quit: our loop ends like the game's own exit (state 6)
int g_bootPhase = 0;  // 0 = waiting for sign-in, 1 = boot gate open
ULONGLONG g_runStartMs = 0;
int g_gateFrames = 0;                // frames since the boot gate opened
ULONGLONG g_phaseStartMs = 0;        // start of the current boot phase
ULONGLONG g_streamMs = 8000;         // ini [game] stream_ms: world streaming time before finishing the load
ULONGLONG g_finishLaterLoadAt = 0;   // later loads (story/missions): when to request the finish (0 = none pending)
// ini [game] title_screen=1: boot to the game's own title screen (PressStart front-end script + "startscreen" movie);
// the world load waits for GameflowRequestWorldLoad (joining a server) instead of starting at the boot gate.
bool g_titleMode = false;
volatile long g_worldLoadRequested = 0;
// Disconnect unloaded the world (GameflowUnloadWorld): the engine's session restart is under way / held, no world is
// drawn or run, until the next join finishes the level load.
bool g_worldParked = false;
// [game] disconnect_unload=1 (development, default 0): Disconnect unloads the world (GameflowUnloadWorld). Off: the world
// stays loaded behind the main menu, the player waits in limbo. Off by default while the empty session's terrain is
// still drawn behind the menu.
bool g_unloadOnDisconnect = false;
bool g_unloadDone = false;  // the unload is complete (empty session, loading-mode render): the title can come back
int g_unloadStep = 0;       // 0 restart pending/running, 1 its load finishing, 2 done (empty session)

uintptr_t Device() { return At<uintptr_t>(G<uintptr_t>(kDeviceHolder), 0x530); }

bool SignedIn() {
    const uintptr_t plat = G<uintptr_t>(kPlatform);
    return plat && F<bool (*)(uintptr_t)>(0x140EF0E70)(plat);
}

// Debug ([debug] input_trace=1): counts PeekMessageW / GetKeyboardState calls per thread and logs one stack of each.
using PeekFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using KbStateFn = BOOL(WINAPI*)(PBYTE);
PeekFn g_realPeek = nullptr;
KbStateFn g_realKbState = nullptr;
volatile long g_peekCalls = 0, g_kbCalls = 0;
void LogStackOnce(const char* what, volatile long& count) {
    if (_InterlockedIncrement(&count) != 50) return;  // after things settled
    void* frames[32] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    char line[1024];
    int len = 0;
    const uintptr_t base = GameBase();
    for (USHORT i = 0; i < n && len < 900; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= base && a < base + 0x3000000) len += sprintf_s(line + len, sizeof(line) - len, " %llx", static_cast<unsigned long long>(a - base + 0x140000000ULL));
    }
    line[len] = 0;
    Log("input trace: %s call #50 on thread %lu (main loop %s):%s", what, GetCurrentThreadId(), g_runStartMs ? "running" : "not started", line);
}
BOOL WINAPI PeekHook(LPMSG m, HWND h, UINT a, UINT b, UINT r) {
    LogStackOnce("PeekMessageW", g_peekCalls);
    return g_realPeek(m, h, a, b, r);
}
BOOL WINAPI KbStateHook(PBYTE s) {
    LogStackOnce("GetKeyboardState", g_kbCalls);
    return g_realKbState(s);
}

// Debug ([debug] radar_trace=1): every 2 s logs the radar/map tile draws (FUN_140690ef0: radar+0x138 tile slots,
// isRadar 1 = minimap / 0 = pause map), the tile range and the live radar zoom (see SetRadarZoom).
constexpr uint8_t kTileDrawPrologue[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x20};
void (*g_tileDrawOrig)(uintptr_t, char, int, int) = nullptr;
volatile long g_tileCalls = 0;
int g_tileMinX = 99, g_tileMaxX = -99, g_tileMinZ = 99, g_tileMaxZ = -99;
void TileDrawHook(uintptr_t radar, char isRadar, int tx, int tz) {
    _InterlockedIncrement(&g_tileCalls);
    if (tx < g_tileMinX) g_tileMinX = tx;
    if (tx > g_tileMaxX) g_tileMaxX = tx;
    if (tz < g_tileMinZ) g_tileMinZ = tz;
    if (tz > g_tileMaxZ) g_tileMaxZ = tz;
    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 2000) {
        last = GetTickCount64();
        const uintptr_t L = G<uintptr_t>(kLoader);
        const uintptr_t zl = L ? At<uintptr_t>(L, 0x25760) : 0;
        const uintptr_t e = zl ? At<uintptr_t>(zl, 8) : 0;
        Log("radar trace: %ld tile draws, x %d..%d z %d..%d isRadar %d, zoom %.1f m", g_tileCalls, g_tileMinX, g_tileMaxX,
            g_tileMinZ, g_tileMaxZ, isRadar, e ? At<float>(e, 0x2a0) : -1.0f);
    }
    g_tileDrawOrig(radar, isRadar, tx, tz);
}
void NoOpWrapperCall(void*) {}

// Radar zoom = the radar's diameter in metres, per HUD entry (list loader+0x25760: +8 entries, stride 0x350, +0x10 u16
// count): entry+0x2a8 is the zoom scripts set (native 0x140654430), entry+0x2a0 the live value the radar camera
// (FUN_140692340: ortho half-size = radar scale 0.8371 * zoom / 0.75 * 0.5) and the blips use; the HUD reset
// FUN_1406399e0 copies +0x2a8 -> +0x2a0. The constructor default is 1.2, i.e. a 1.2 m wide radar = one texel of the
// map texture: the radar looks blank until a script sets a real zoom, and our boot runs no story script.
float g_radarZoomM = 150.0f;  // ini [sandbox] radar_zoom
bool g_noPauseFreeze = true;  // ini [sandbox] no_pause_freeze: the world keeps running behind the pause menu
bool g_pauseTrace = false;    // ini [debug] pause_trace: log game time once a second while paused
void SetRadarZoom(uintptr_t loader, float metres) {
    const uintptr_t list = At<uintptr_t>(loader, 0x25760);
    const uintptr_t entries = list ? At<uintptr_t>(list, 8) : 0;
    if (!entries) return;
    const int count = At<uint16_t>(list, 0x10);
    for (int i = 0; i < count; ++i) {
        At<float>(entries + i * 0x350, 0x2a8) = metres;
        At<float>(entries + i * 0x350, 0x2a0) = metres;
    }
    Log("gameflow: radar zoom %.0f m (%d HUD entries)", metres, count);
}

// Our world-load start (replaces the front end's path: title -> menus -> save checks -> loading screen):
//   * DAT_142AC2956 = 1 + FUN_140637520(loader) - what the game does on "fileStartupChecksComplete": HUD map init
//     (minimap shader, blips) and, inside it, FUN_14011a310 = load requests for all UI movies (HUD, fader, ...).
//   * game+0x12e |= 1 ("new game") - the per-frame level-load trigger FUN_1401f38d0 then runs the level load.
// Not called: FUN_14063a900 (loading screen setup: brush art / revolver movies, LoadingScreen scene).
void BeginWorldLoad() {
    const uintptr_t loader = G<uintptr_t>(kLoader);
    const uintptr_t game = G<uintptr_t>(0x142C456D0);
    G<uint8_t>(0x142AC2956) = 1;
    // World streaming start: what the frame update FUN_1401f3ad0 does once the title screen ("StartScreen1") shows -
    // without it the world grid (*0x142AC5098: collision / active world cells) never streams in.
    if (const uintptr_t sc = G<uintptr_t>(0x142AC5140); sc && At<uint8_t>(sc, 0x124)) {
        At<uint8_t>(sc, 0x124) = 0;
        G<uint32_t>(0x1422A3E2C) = 1;
        if (const uintptr_t grid = G<uintptr_t>(0x142AC5098)) At<uint8_t>(grid, 0x28) = 0;
        At<uint64_t>(sc, 0x70) = 0x42c80000;  // 100.0f, 0
        At<uint32_t>(sc, 0x78) = 0;
        At<uint32_t>(sc, 0x7c) = 0x41200000;  // 10.0f
        At<uint8_t>(sc, 0x11f) = 0;
        Log("gameflow: world streaming started");
    }
    // The boot UI scene "Startup" is closed by the front end before play; it stays on top of the HUD otherwise.
    if (const uintptr_t ui = G<uintptr_t>(kUiMgr)) {
        if (const uintptr_t root = At<uintptr_t>(ui, 0xa0)) {
            const uintptr_t startup = F<uintptr_t (*)(uintptr_t, const char*)>(0x1400EF100)(root, "Startup");
            if (startup && V<bool (*)(uintptr_t, int)>(startup, 0xd8)(startup, 1)) {
                V<void (*)(uintptr_t)>(startup, 0x158)(startup);
                Log("gameflow: closed the Startup UI scene");
            }
        }
    }
    // UI notifications the front end posts when it hands over to the game (the HUD movie listens for them).
    PostUiEvent("startScreenExit");
    PostUiEvent("mainMenuExit");
    F<void (*)(uintptr_t)>(0x140637520)(loader);
    At<uint8_t>(game, 0x12e) |= 1;
    SandboxMarkInGame();
}

// Debug ([debug] map_trace=1): pause-map state once a second while it changes - open flag 0x142AC50BB, render gates
// 0x142AC52A8 / 0x142AC5257 (<- 52AA), map object loader+0xc550: +0x40 txd slot (worldMap_LOD_NN), +0x4c its flags.
bool g_mapTrace = false;
void MapTrace() {
    static ULONGLONG last = 0;
    static char prev[200] = {};
    if (GetTickCount64() - last < 1000) return;
    last = GetTickCount64();
    const uintptr_t loader = G<uintptr_t>(kLoader);
    if (!loader) return;
    const uintptr_t m = loader + 0xc550;
    // "blips" UI resource (list *0x142C45608: +0x138 entries stride 0x60, +0 hash, +8 loaded, +9 wanted; +0x6f8 count)
    int blipsIdx = -1, blipsLoaded = -1, blipsWanted = -1;
    if (const uintptr_t rl = G<uintptr_t>(0x142C45608)) {
        const uint32_t h0 = F<uint32_t (*)(const char*, int)>(0x1400C7510)("blips", 0);
        const uint32_t h = ((h0 * 9) >> 0xb ^ h0 * 9) * 0x8001;
        const int n = At<int32_t>(rl, 0x6f8);
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            Log("map trace: UI resource list %p count %d (blips hash %08x)", reinterpret_cast<void*>(rl), n, h);
            for (int i = 0; i < n && i < 64; ++i) {
                const uintptr_t e = rl + 0x138 + i * 0x60;
                Log("  res[%d] hash %08x +8=%d +9=%d +10=%p name?=\"%.24s\"", i, At<uint32_t>(e, 0), At<uint8_t>(e, 8), At<uint8_t>(e, 9),
                    reinterpret_cast<void*>(At<uintptr_t>(e, 0x10)), reinterpret_cast<const char*>(e + 0x18));
            }
        }
        for (int i = 0; i < n && i < 256; ++i) {
            const uintptr_t e = rl + 0x138 + i * 0x60;
            if (At<uint32_t>(e, 0) == h) { blipsIdx = i; blipsLoaded = At<uint8_t>(e, 8); blipsWanted = At<uint8_t>(e, 9); break; }
        }
    }
    char cur[200];
    sprintf_s(cur, "blips[%d] loaded=%d wanted=%d | 50bb=%d 52a8=%d 5257=%d 52aa=%d 5256=%d txd=%p fl=%08x lod1=%p proxy.cnt=%d",
              blipsIdx, blipsLoaded, blipsWanted,
              G<uint8_t>(0x142AC50BB), G<uint8_t>(0x142AC52A8), G<uint8_t>(0x142AC5257), G<uint8_t>(0x142AC52AA),
              G<uint8_t>(0x142AC5256), reinterpret_cast<void*>(At<uintptr_t>(m, 0x40)), At<uint32_t>(m, 0x4c),
              reinterpret_cast<void*>(G<uintptr_t>(0x142AC51C0)),
              At<uintptr_t>(loader, 0xc3d0) ? At<int8_t>(At<uintptr_t>(loader, 0xc3d0), 0x40) : -99);
    if (std::strcmp(cur, prev) != 0) {
        strcpy_s(prev, cur);
        Log("map trace: %s", cur);
    }
}

// The loader's post-loading-screen cleanup (FUN_14063ac00, flag loader+50000) pops the top UI phases, which includes
// "effects"; without the loading screen we drop it the same way (count-1, clear the slot). It covers all UI otherwise.
void DropEffectsUiPhase() {
    if (const uintptr_t ui = G<uintptr_t>(0x142C45610)) {
        int& count = At<int32_t>(ui, 0x180);
        const uintptr_t top = count > 0 ? At<uintptr_t>(ui, 0x30 + (count - 1) * 8) : 0;
        if (top && std::strncmp(reinterpret_cast<const char*>(top + 0x68), "effects", 8) == 0) {
            At<uintptr_t>(ui, 0x30 + (count - 1) * 8) = 0;
            --count;
            Log("gameflow: dropped the top UI phase \"effects\"");
        }
    }
}

// Holds the loader's fader at full black: the level-load trigger FUN_1401f38d0 waits for it (normally the loading
// screen's ActionScript fades it in).
void HoldLoaderFaderBlack(uintptr_t loader) {
    At<float>(loader, 0x25800) = 1.0f;
    At<uint8_t>(loader, 0x257ec) |= 1;
    At<uint8_t>(loader, 0x257e5) &= static_cast<uint8_t>(~0x48);
}

// Our boot sequence, run once per frame before Game::Update.
void BootStep() {
    if (g_titleMode) UiFilesFrame(g_bootPhase < 2);
    if (g_mapTrace && g_bootPhase >= 5) MapTrace();
    if (g_bootPhase == 0 && SignedIn()) {
        // The boot gate in Game::Update (FUN_1401f5820): (legals controller == 0 || done) && uiMgr+0x78. With no
        // controller only uiMgr+0x78 is left - the flag the logo video's end used to set.
        const uintptr_t ui = G<uintptr_t>(kUiMgr);
        if (!ui) return;
        // Title mode keeps the game's legals/logo controller, whose logo end opens the gate itself.
        if (!g_titleMode) _InterlockedExchange8(reinterpret_cast<volatile char*>(ui + 0x78), 1);
        g_bootPhase = 1;
        Log("gameflow: signed in after %llu ms of our loop -> boot gate open", GetTickCount64() - g_runStartMs);
    }
    if (g_bootPhase == 1 && g_worldParked) {
        // After a Disconnect (GameflowUnloadWorld): (1) the session restart runs once the trigger sees the fader black;
        // (2) its load is finished at once - an empty session (no player: the sandbox does not spawn one while parked;
        // no script runs: mainthread.cpp); our main menu over it. A join then starts another restart and runs the boot's
        // load phases (2..5).
        const uintptr_t game = G<uintptr_t>(0x142C456D0);
        const uintptr_t loader = G<uintptr_t>(kLoader);
        if (!game || !loader) return;
        if (At<uint8_t>(game, 0x12e) & 1) {
            HoldLoaderFaderBlack(loader);
            return;
        }
        if (g_unloadStep == 0) {
            DropEffectsUiPhase();  // (the restart's load pushed it)
            RequestFinishLoading("unload: empty session");
            g_unloadStep = 1;
            return;
        }
        if (g_unloadStep == 1) {
            if (At<uint8_t>(loader, 0x257e5) & 0x10) return;  // (finish request pending until "loadComplete")
            g_unloadStep = 2;
            g_unloadDone = true;
            Log("gameflow: world unloaded (empty session loaded)");
        }
        // (Not hidden yet: switching the renderer to loading mode outside a level load - the handshake of FUN_1401f85b0 -
        // was undone within ~4 ms and killed the game, also when done once ~1 s after the hand-back. The empty session's
        // terrain stays drawn behind the menu; see re/NOTES.md "Disconnect".)
        if (!g_worldLoadRequested) return;
        // Join: a fresh session and the boot's load (phase 2 holds the fader until the trigger ran, then 3..5).
        g_unloadStep = 0;
        g_unloadDone = false;
        g_worldParked = false;
        At<uint8_t>(game, 0x12e) |= 1;
        g_bootPhase = 2;
        g_phaseStartMs = GetTickCount64();
        Log("gameflow: join after a Disconnect - loading a new session");
        return;
    }
    if (g_bootPhase == 1 && g_titleMode && !g_worldLoadRequested) return;  // title screen: wait for a join
    if (g_bootPhase == 1 && ++g_gateFrames >= 2) {
        // What the PressStart front-end script + title + menus used to lead to: start the world load directly.
        g_bootPhase = 2;
        Log("gameflow: starting the world load");
        BeginWorldLoad();
        g_phaseStartMs = GetTickCount64();
        return;
    }
    const uintptr_t loader = G<uintptr_t>(kLoader);
    const uintptr_t game = G<uintptr_t>(0x142C456D0);
    if (g_bootPhase == 2 && loader && game) {
        // The level-load trigger FUN_1401f38d0 waits until the loader's fader is at full black (normally the loading
        // screen's ActionScript fades it in). Hold it there ourselves until the trigger has run (new-game bit cleared).
        if ((At<uint8_t>(game, 0x12e) & 1) == 0) {
            DropEffectsUiPhase();
            g_bootPhase = 3;
            g_phaseStartMs = GetTickCount64();
            Log("gameflow: level load running (%llu ms after start)", GetTickCount64() - g_runStartMs);
            return;
        }
        HoldLoaderFaderBlack(loader);
    }
    if (g_bootPhase == 3 && GetTickCount64() - g_phaseStartMs >= g_streamMs) {
        // Streaming time for the area around the spawn, then finish the load (FinishLoading -> "loadComplete").
        g_bootPhase = 4;
        RequestFinishLoading("our boot");
        return;
    }
    if (g_bootPhase >= 5 && g_finishLaterLoadAt && GetTickCount64() >= g_finishLaterLoadAt && loader) {
        // later loads (story / missions): there is no loading screen to end them, so request the finish (kept set
        // every frame until "loadComplete", like the loading screen does).
        At<uint8_t>(loader, 0x257e5) |= 0x10;
    }
    if (g_bootPhase == 4 && loader && (At<uint8_t>(loader, 0x257e5) & 0x10) == 0) {
        // Load finished. HUD on: loader+0x257c0 is the HUD/minimap draw switch (checked by the render FUN_1405c8720),
        // cleared by the level load; the front-end script turned it back on via native 0x140655680.
        At<uint8_t>(loader, 0x257c0) = 1;
        // Radar map on: the radar streams/draws its map tiles ("streaming_worldMap_N") only with these flags set;
        // story scripts set them via natives, sp_idle never does.
        G<uint8_t>(0x14227D32E) = 1;  // radar streams its map tiles (FUN_140691ca0); native 0x140653B40
        G<uint8_t>(0x14227D32F) = 1;  // radar draws the map (-> DAT_14227cbff, FUN_140692340); native 0x140654DB0
        SetRadarZoom(loader, g_radarZoomM);
        // Pause map: its render gate (0x142AC5257 <- 52AA, set in FUN_140603410) also waits for the UI texture
        // dictionary "blips" (blip icons) to be loaded - story scripts request it via a native (FUN_1400fba10 =
        // request a UI txd by name into the list at *0x142C45608); our boot runs no script, so request it here.
        if (const uintptr_t uiTxd = G<uintptr_t>(0x142C45608)) {
            F<void (*)(uintptr_t, const char*)>(0x1400FBA10)(uiTxd, "blips");
            Log("gameflow: requested UI txd \"blips\" (pause map)");
        }
        g_bootPhase = 5;
        Log("gameflow: in game, HUD on (%llu ms after our loop started)", GetTickCount64() - g_runStartMs);
        if (g_titleMode) PostUiEvent("rdmp.ingame");  // the HUD takes the UI stack from our front end (uifiles.cpp)
    }
}

// Releases the device from this thread if it owns it (what Game::Run does before kicking the render thread).
void ReleaseDeviceOwnership() {
    const uint32_t tid = F<uint32_t (*)()>(0x140EB9090)();
    if (G<uint64_t>(kRenderThreadOwner) != tid) return;
    auto** tls = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
    if (tls[0][0x38] == 0) F<void (*)()>(0x140F49E08)();  // __dyn_tls_on_demand_init
    F<void (*)(uint64_t)>(0x140EC6E50)(*reinterpret_cast<uint64_t*>(tls[0] + 0x28));
    G<uint64_t>(kRenderThreadOwner) = ~0ULL;
}

// Replacement for FUN_1405f4390 (loading-mode frame, render thread / init): begin, bookkeeping, black, present.
void OurLoadingFrame() {
    const uintptr_t frame = G<uintptr_t>(kGfxFrame);
    V<void (*)(uintptr_t, int)>(frame, 0x20)(frame, 0);
    uintptr_t rm = G<uintptr_t>(kRenderMgr);
    const uintptr_t streamer = G<uintptr_t>(kStreamer);
    auto wait = [](uintptr_t h) { if (h) WaitForSingleObject(reinterpret_cast<HANDLE>(h), INFINITE); };
    wait(At<uintptr_t>(rm, 0x48));
    if (streamer) {
        F<void (*)(uintptr_t)>(0x140EB4070)(streamer + 0x1cb0);  // enter
        F<void (*)()>(0x14053AB10)();
        F<void (*)(uintptr_t, uint64_t, int, uint8_t, uint8_t)>(0x140517A50)(streamer, 0, At<uint16_t>(rm, 0xa8), 0, 1);
        F<void (*)(uintptr_t)>(0x140EB40A0)(streamer + 0x1cb0);  // leave
    }
    F<void (*)()>(0x140EBF550)();  // deferred GPU resource release
    rm = G<uintptr_t>(kRenderMgr);
    if (const uintptr_t h = At<uintptr_t>(rm, 0x48)) ReleaseMutex(reinterpret_cast<HANDLE>(h));
    F<void (*)(uint32_t, uint32_t, uint8_t, float, uint8_t, uint64_t)>(0x14012DF00)(1, 0xff000000u, 0, 1.0f, 1, 0);  // black
    // UI advance only (no UI draw): Scaleform movies (HUD, fader, ...) load and tick inside their Advance, which in
    // loading mode only runs from here (FUN_140119f10 with ui+0x1e0 set).
    if (const uintptr_t ui = G<uintptr_t>(0x142C45610); ui && G<uintptr_t>(kCamMgr)) {
        rm = G<uintptr_t>(kRenderMgr);
        wait(At<uintptr_t>(rm, 0x58));
        At<uint8_t>(ui, 0x1e0) = 1;
        F<void (*)(uintptr_t)>(0x140119F10)(ui);
        if (const uintptr_t h = At<uintptr_t>(G<uintptr_t>(kRenderMgr), 0x58)) ReleaseMutex(reinterpret_cast<HANDLE>(h));
    }
    G<uint32_t>(0x142AC5100) = G<uint32_t>(0x14227D26C);
    V<void (*)(uintptr_t)>(frame, 0x28)(frame);  // end + present
    if (const uintptr_t s = G<uintptr_t>(kStreamer)) At<uint8_t>(s, 0x1ce8) = 1;
}

// UI alert update FUN_140330a60 (per frame, inside Game::Update). Its tail raises "SocialClubReturnToMenu" ("You have been
// logged out of Rockstar Games. Returning to main menu.") when the game's sign-in check FUN_140ef0e70 is false and the legals
// controller (*0x142AC5130) is absent or done and no title screen is up. The real boot runs ~7 s of legal pages first, so the
// sign-in (progressed by the game loop) is always finished before that check can fire. Our boot has no legals controller,
// so the check ran from the first frame and sometimes beat the sign-in by a few ms. During boot phase 0 (waiting for the
// sign-in; the world doesn't load without it) this update is held back, like the legal pages did; capped at 20 s, and
// once signed in it runs every frame as normal, so a real sign-out later still shows the box.
constexpr uint8_t kUiAlertPrologue[] = {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x89, 0x68, 0xE8};
void (*g_uiAlertOrig)(uintptr_t, float) = nullptr;
void UiAlertUpdateHook(uintptr_t self, float dt) {
    if (g_bootPhase == 0 && !SignedIn() && (!g_runStartMs || GetTickCount64() - g_runStartMs < 20000)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("gameflow: UI alert update held until the sign-in finishes (legals stand-in)");
        }
        return;
    }
    g_uiAlertOrig(self, dt);
}

// Replacement for Game::Run (FUN_1401f3140).
void OurRun(uintptr_t game) {
    g_runStartMs = GetTickCount64();
    Log("gameflow: our game loop running");
    uintptr_t rm = G<uintptr_t>(kRenderMgr);
    F<void (*)(uintptr_t, int)>(0x140681390)(G<uintptr_t>(0x142AC50A0) + 0x1c8d8, 0);
    auto activeCam = [] {
        const uintptr_t cm = G<uintptr_t>(kCamMgr);
        return At<uintptr_t>(cm, 8 + static_cast<int64_t>(At<int32_t>(cm, 0x48)) * 8);
    };
    F<void (*)(uintptr_t, uintptr_t)>(0x140394DE0)(rm + 0x870, activeCam() + 0x190);
    {
        const uintptr_t src = activeCam() + 0x1d0;
        G<uintptr_t>(0x142C3F7D0) = rm + 0x870;
        for (int row = 0; row < 4; ++row)
            for (int c = 0; c < 3; ++c) At<uint32_t>(rm, 0xce0 + row * 0x10 + c * 4) = At<uint32_t>(src, row * 0x10 + c * 4);
    }
    F<void (*)(uintptr_t)>(0x14063AC00)(G<uintptr_t>(kLoader));
    {
        const uintptr_t l = G<uintptr_t>(0x142C456C0);
        const uintptr_t cb = GameAddr(0x14062BA60);
        auto run = F<void (*)(uintptr_t, uintptr_t, uintptr_t)>(0x14062E190);
        run(At<uintptr_t>(At<uintptr_t>(l, 0x20), 0), At<uintptr_t>(l, 0x20), cb);
        run(At<uintptr_t>(At<uintptr_t>(l, 0x68), 0), At<uintptr_t>(l, 0x68), cb);
    }
    {
        const uintptr_t dflt = GameAddr(0x141C7AAB0);
        G<uintptr_t>(0x142B0F070) = G<uintptr_t>(0x142B66968) == 0 ? G<uintptr_t>(0x142B0F070) : dflt;
        G<uintptr_t>(0x142B0F000) = G<uintptr_t>(0x142B0F038) != 0 ? G<uintptr_t>(0x142B0F000) : dflt;
    }
    At<int32_t>(At<uintptr_t>(game, 8), 0) = 0;
    if (G<uintptr_t>(0x142B0E440) != 0) At<int32_t>(At<uintptr_t>(game, 8), 0) = 5;
    // Game::Run starts the front-end script "$/content/PressStart" here (title screen and its menus) and keeps its
    // thread id at game+0x144: t = FUN_1401e9c30(); FUN_140205900(t, path, 0x600, 0, 0); FUN_1402060b0(t, 0, 0, 0, 0).
    At<int32_t>(game, 0x144) = 0;
    if (g_titleMode) {
        const uintptr_t t = F<uintptr_t (*)()>(0x1401E9C30)();
        F<void (*)(uintptr_t, const char*, int, uint64_t, int)>(0x140205900)(t, "$/content/PressStart", 0x600, 0, 0);
        F<void (*)(uintptr_t, uint64_t, uint64_t, uint64_t, uint64_t)>(0x1402060B0)(t, 0, 0, 0, 0);
        At<int32_t>(game, 0x144) = At<int32_t>(t, 0x374);
        Log("gameflow: front-end script PressStart started (thread %d)", At<int32_t>(game, 0x144));
    }

    // FUN_1400B70D0 keeps the previous frame time in its argument (Game::Run passes the same stack slot every frame).
    alignas(16) uint8_t frameTimer[16] = {};
    auto& state = G<int32_t>(0x1422D9BD0);
    auto& nextState = G<int32_t>(0x1422D9C0C);
    auto& stateChange = G<uint8_t>(0x1422D9C08);
    for (;;) {
        rm = G<uintptr_t>(kRenderMgr);
        _InterlockedExchange(reinterpret_cast<volatile long*>(GameAddr(0x142AB32FC)), At<uint8_t>(rm, 0x34) == 0);
        F<void (*)(void*)>(0x1400B70D0)(frameTimer);

        // game state machine (as in Game::Run)
        {
            const uintptr_t loader = G<uintptr_t>(kLoader);
            auto fading = [loader] {
                const uint8_t e5 = At<uint8_t>(loader, 0x257e5);
                return ((e5 & 0x48) == 0 && (At<uint8_t>(loader, 0x257ec) & 1)) || (static_cast<int8_t>(e5) < 0 && !(e5 & 0x40));
            };
            bool toCheck = false;
            int32_t edx = 0;
            if (G<uint8_t>(0x142AC2927) != 0 && state != 5 && fading()) {
                nextState = 5;
                stateChange = 1;
                At<uint8_t>(G<uintptr_t>(0x142AD3238), 0x6398) = 0;
            }
            bool decided = false;
            if (state == 5) {
                if (!fading() || F<bool (*)(uintptr_t, int)>(0x1405F73C0)(G<uintptr_t>(0x142AC5140), 6)) {
                    G<uint8_t>(0x142AC2927) = 0;
                    stateChange = 1;
                    edx = static_cast<int8_t>(At<uint8_t>(G<uintptr_t>(0x142AC4748), 0x3df)) > 0 ? 4 : 3;
                    nextState = edx;
                    toCheck = decided = true;
                }
            }
            if (!decided && stateChange) {
                edx = nextState;
                toCheck = true;
            }
            if (toCheck && edx != state) {
                state = edx;
                stateChange = 0;
            }
        }
        const bool exitAfter = state == 6 || g_quitRequested;

        BootStep();

        rm = G<uintptr_t>(kRenderMgr);
        if (const uintptr_t h = At<uintptr_t>(rm, 0x50)) WaitForSingleObject(reinterpret_cast<HANDLE>(h), INFINITE);
        uintptr_t dev = Device();
        V<void (*)(uintptr_t, uint8_t)>(dev, 0xf0)(dev, 1);
        F<void (*)()>(0x140C68920)();
        // Pause menu without freezing the world (multiplayer): the pause menu sets game state 4 (FUN_140331580, pause
        // counter uiObj+0x3df); Game::Update then skips the world simulation unless it's a net session (state 4 &&
        // net+0x1634 && !DAT_142b0ebc8 - the MP rule). Present state 3 for the Update call only, then put 4 back, so
        // the menu, the renderer and the unpause logic still see "paused".
        const bool unfreeze = g_noPauseFreeze && state == 4;
        if (unfreeze) state = 3;
        V<void (*)(uintptr_t, float)>(game, 0x28)(game, G<float>(0x142314D08));  // Game::Update
        if (unfreeze && state == 3) state = 4;
        if (g_pauseTrace && state == 4) {
            static ULONGLONG lastPt = 0;
            if (GetTickCount64() - lastPt > 1000) {
                lastPt = GetTickCount64();
                Log("pause trace: state 4, game time %.2f, dt %.4f, next %d/%d", G<float>(0x142314D3C), G<float>(0x142314D08), nextState, stateChange);
            }
        }
        {
            const uintptr_t r = F<uintptr_t (*)()>(0x140EE6300)();
            const uintptr_t a = At<uintptr_t>(r, 0x18);
            uintptr_t sel;
            if (a) sel = (At<uint8_t>(a, 8) == 0 && At<uintptr_t>(r, 0x20)) ? At<uintptr_t>(r, 0x20) : a;
            else sel = At<uintptr_t>(r, 0x20);
            dev = Device();
            if (At<uint8_t>(sel, 0x15)) V<void (*)(uintptr_t)>(dev, 0x120)(dev);
            const uintptr_t r2 = F<uintptr_t (*)()>(0x140EE6300)();
            if (At<uint8_t>(At<uintptr_t>(r2, 0x10), 0x26c)) V<void (*)(uintptr_t, int)>(dev, 0x128)(dev, 0);
            V<void (*)(uintptr_t)>(dev, 0xf8)(dev);
        }
        rm = G<uintptr_t>(kRenderMgr);
        const bool single = G<uintptr_t>(0x142B66818) != 0;
        if (At<uint8_t>(rm, 0x34) == 0 && (!single || At<uint8_t>(rm, 0x6db)) && At<uint8_t>(rm, 0x53d)) {
            if (const uintptr_t ev = At<uintptr_t>(rm, 0x18)) F<bool (*)(uintptr_t, int)>(0x140EBD8C0)(ev, -1);
            At<uint8_t>(rm, 0x53d) = 0;
        }
        _InterlockedExchange(reinterpret_cast<volatile long*>(GameAddr(0x142AB3304)), 0);
        // FUN_1405c5220 advances the game clock (0x142314D3C) with the same MP rule - same treatment as Update.
        const bool unfreezeTimer = g_noPauseFreeze && state == 4;
        if (unfreezeTimer) state = 3;
        F<void (*)(uintptr_t)>(0x1405C5220)(rm);
        if (unfreezeTimer && state == 3) state = 4;
        if (const uintptr_t o = At<uintptr_t>(game, 0xd8)) {
            const uintptr_t u = G<uintptr_t>(0x142AAC8D0);
            const uint8_t flag = (At<uint8_t>(u, 0x138) != 0 && At<int32_t>(At<uintptr_t>(u, 0), 0x30) != 5) ? 1 : 0;
            F<void (*)(uintptr_t, uint8_t)>(0x14007C4B0)(o, flag);
            const uintptr_t o2 = At<uintptr_t>(game, 0xd8);
            if (At<uint8_t>(o2, 0x291)) {
                if (o2) V<void (*)(uintptr_t, int)>(o2, 0)(o2, 1);
                At<uintptr_t>(game, 0xd8) = 0;
            }
        }
        {
            const uintptr_t sys = F<uintptr_t (*)()>(0x140EBF9C0)();
            if (At<uint8_t>(sys, 0xb2)) break;
            const uintptr_t sys2 = F<uintptr_t (*)()>(0x140EBF9C0)();
            if (sys2 && At<uint8_t>(sys2, 0xaf)) break;
        }
        G<float>(0x142AC52E4) = G<float>(0x142314D3C);
        rm = G<uintptr_t>(kRenderMgr);
        {
            const uintptr_t s = G<uintptr_t>(0x142AC50A0);
            if (uintptr_t pending = At<uintptr_t>(s, 0x1c8a8)) {
                F<void (*)(int, uintptr_t*)>(0x1400B9060)(1, &pending);
                At<uintptr_t>(s, 0x1c8a8) = 0;
            }
        }
        F<void (*)(uintptr_t, uintptr_t, uintptr_t)>(0x1408604A0)(rm + 0x1e0, rm + 0x870, rm + 0xce0);
        G<uintptr_t>(0x142C3F7D0) = rm + 0x870;
        rm = G<uintptr_t>(kRenderMgr);
        if (At<uint8_t>(rm, 0x34) == 0 && (!single || At<uint8_t>(rm, 0x6db))) {
            F<void (*)(uintptr_t)>(0x1405C7920)(rm);  // build the frame
            rm = G<uintptr_t>(kRenderMgr);
            if (single) {
                if (At<uint8_t>(rm, 0x34)) OurLoadingFrame();
                else if (At<uint8_t>(rm, 0x6db)) F<void (*)(uintptr_t)>(0x1405C8720)(rm);
            } else if (At<uint8_t>(rm, 0x34) == 0) {
                ReleaseDeviceOwnership();
                if (const uintptr_t ev = At<uintptr_t>(rm, 0x10)) F<void (*)(uintptr_t)>(0x140EBD830)(ev);  // kick render thread
                At<uint8_t>(rm, 0x53d) = 1;
            }
            rm = G<uintptr_t>(kRenderMgr);
        }
        if (const uintptr_t h = At<uintptr_t>(rm, 0x50)) {
            ReleaseMutex(reinterpret_cast<HANDLE>(h));
            rm = G<uintptr_t>(kRenderMgr);
        }
        G<uint8_t>(0x142AE03DE) = At<uint8_t>(rm, 0x34);
        F<void (*)()>(0x1405FECF0)();
        dev = Device();
        if (V<bool (*)(uintptr_t)>(dev, 0xc0)(dev)) F<void (*)(uintptr_t)>(0x140F9C5F0)(dev);
        if (exitAfter) break;
    }
    Log("gameflow: our game loop exiting");
    NetShutdown();
    if (g_quitRequested) {
        // The menu's Quit ends the process like the game's own Quit does (ExitProcess with the level loaded): the
        // engine's shutdown below waits forever with a world loaded.
        Log("gameflow: quitting");
        ExitProcess(0);
    }
    int32_t us = 100000;
    F<void (*)(int32_t*)>(0x140EB9160)(&us);
    const uintptr_t sysApp = At<uintptr_t>(G<uintptr_t>(0x142893448), 0x48);
    V<void (*)(uintptr_t)>(sysApp, 0x10)(sysApp);
}

}  // namespace

bool OwnGameLoopActive() { return g_installed; }
void GameflowRequestQuit() {
    if (!g_installed) return;
    Log("gameflow: quit requested (menu)");
    g_quitRequested = true;
}
bool OwnBootGateOpen() { return g_bootPhase >= 1; }
bool GameflowTitleMode() { return g_titleMode; }
bool GameflowWorldLoadStarted() { return g_bootPhase >= 2; }
// Disconnect: unload the world in-process. The game itself never unloads a level (its "return to main menu" restarts
// the process through PlayRDR.exe -restart; quitting calls ExitProcess with the level loaded), but its level load
// restarts the session when one exists: FUN_1401f38d0 (game+0x12e bit 0) -> FUN_1401f85b0 (streaming drained, renderer
// to loading mode rm+0x34 = 1) -> FUN_1401f8a00, which with a session (0x142AC2925) first releases it (session objects,
// actors - the player too, scripts, world state; the block from 0x1401F8BE3) and then sets up a fresh one. We run that
// restart and finish its load at once: an empty session (no player, no actors, no scripts) under our main menu
// (BootStep); the next join restarts the session again with the boot's load phases.
void GameflowUnloadWorld() {
    if (!g_installed || !g_titleMode || g_bootPhase < 5) return;
    const uintptr_t game = G<uintptr_t>(0x142C456D0);
    if (!game) return;
    At<uint8_t>(game, 0x12e) |= 1;
    g_worldParked = true;
    g_unloadStep = 0;
    g_unloadDone = false;
    g_bootPhase = 1;
    g_gateFrames = 0;
    g_finishLaterLoadAt = 0;
    _InterlockedExchange(&g_worldLoadRequested, 0);
    SandboxWorldUnloaded();
    NetWorldUnloaded();
    Log("gameflow: unloading the world (Disconnect)");
}
bool GameflowWorldParked() { return g_worldParked; }
bool GameflowUnloadOnDisconnect() { return g_installed && g_titleMode && g_unloadOnDisconnect; }
bool GameflowUnloadDone() { return g_worldParked && g_unloadDone; }

void GameflowRequestWorldLoad() {
    if (_InterlockedExchange(&g_worldLoadRequested, 1) == 0) Log("gameflow: world load requested");
}
void GameflowOnUiEvent(const char* name) {
    if (!g_installed || g_bootPhase < 5) return;
    if (_stricmp(name, "loadStart") == 0 && !g_finishLaterLoadAt) {
        g_finishLaterLoadAt = GetTickCount64() + g_streamMs;
        Log("gameflow: later load started - finishing it in %llu ms", g_streamMs);
    } else if (std::strcmp(name, "loadComplete") == 0 && g_finishLaterLoadAt) {
        g_finishLaterLoadAt = 0;
        Log("gameflow: later load complete");
    }
}

void InstallGameFlow(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"debug", L"input_trace", 0, ini)) {
        g_realPeek = reinterpret_cast<PeekFn>(PatchImport("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&PeekHook)));
        g_realKbState = reinterpret_cast<KbStateFn>(PatchImport("USER32.dll", "GetKeyboardState", reinterpret_cast<void*>(&KbStateHook)));
        Log("input trace: PeekMessageW %s, GetKeyboardState %s", g_realPeek ? "hooked" : "-", g_realKbState ? "hooked" : "-");
    }
    if (GetPrivateProfileIntW(L"debug", L"radar_trace", 0, ini)) {
        g_tileDrawOrig = reinterpret_cast<void (*)(uintptr_t, char, int, int)>(DetourPic(GameAddr(0x140690EF0), kTileDrawPrologue, sizeof(kTileDrawPrologue), reinterpret_cast<void*>(&TileDrawHook)));
        Log("radar trace: %s", g_tileDrawOrig ? "on" : "detour FAILED");
    }
    if (!GetPrivateProfileIntW(L"game", L"own_loop", 0, ini)) return;
    g_streamMs = GetPrivateProfileIntW(L"game", L"stream_ms", 8000, ini);
    g_titleMode = GetPrivateProfileIntW(L"game", L"title_screen", 0, ini) != 0;
    g_unloadOnDisconnect = GetPrivateProfileIntW(L"game", L"disconnect_unload", 0, ini) != 0;
    g_radarZoomM = static_cast<float>(GetPrivateProfileIntW(L"sandbox", L"radar_zoom", 150, ini));
    g_mapTrace = GetPrivateProfileIntW(L"debug", L"map_trace", 0, ini) != 0;
    g_noPauseFreeze = GetPrivateProfileIntW(L"sandbox", L"no_pause_freeze", 1, ini) != 0;
    g_pauseTrace = GetPrivateProfileIntW(L"debug", L"pause_trace", 0, ini) != 0;
    bool ok = true;
    // 1. Game::Init: skip the legals/logo controller (not in title mode: the game's own boot runs - legal pages,
    //    logo, loading screen, title) (alloc 0x38 + ctor FUN_1405f4690 + start/free, 0x1401EF9D7 ..
    //    0x1401EFA21). The UI renderer setup just before it (FUN_14011a220 at 0x1401EF9D2) must still run.
    if (!g_titleMode) {
        const uint8_t expect[5] = {0xE8, 0x94, 0x03, 0xE9, 0xFF};  // CALL FUN_14007fd70 (allocator)
        const uint8_t jmp[2] = {0xEB, 0x48};                       // JMP 0x1401EFA21
        const bool match = std::memcmp(reinterpret_cast<void*>(GameAddr(0x1401EF9D7)), expect, sizeof(expect)) == 0;
        ok &= match && WriteCode(GameAddr(0x1401EF9D7), jmp, sizeof(jmp));
        Log("gameflow: init without legals/logo controller: %s", match ? "ok" : "bytes mismatch");
    }
    // 2. Loading-mode frame.
    if (GetPrivateProfileIntW(L"game", L"replace_loading_frame", g_titleMode ? 0 : 1, ini)) {
        const uint8_t expect[7] = {0x48, 0x81, 0xEC, 0xB8, 0x04, 0x00, 0x00};
        const bool match = std::memcmp(reinterpret_cast<void*>(GameAddr(0x1405F4390)), expect, sizeof(expect)) == 0;
        ok &= match && ReplaceFunction(GameAddr(0x1405F4390), reinterpret_cast<void*>(&OurLoadingFrame));
        Log("gameflow: loading frame replaced: %s", match ? "ok" : "bytes mismatch");
    }
    // 3. Game::Run -> ours (game vtable slot +0x10).
    {
        auto* slot = reinterpret_cast<uintptr_t*>(GameAddr(kGameVtable + 0x10));
        const bool match = *slot == GameAddr(0x1401F3140);
        const uintptr_t ours = reinterpret_cast<uintptr_t>(&OurRun);
        ok &= match && WriteCode(reinterpret_cast<uintptr_t>(slot), &ours, sizeof(ours));
        Log("gameflow: game loop replaced: %s", match ? "ok" : "vtable mismatch");
    }
    // 4. Loading screen + title screen movies: their wrapper classes (created by the loader's init FUN_140637a00 and
    //    pushed into the UI list) get no-op Advance (+0x18, also where the movie loads) and Draw (+0x28), so the
    //    LoadingScreen / LoadingSpinner / StartScreen Flash movies are never loaded, ticked or drawn.
    //    "loading" vt 0x141CE4118 (loader+0xc3f0/+0xc3f8), "startscreen" vt 0x141CE5318 (loader+0xc4a0).
    static const uintptr_t kVts[] = {0x141CE4118, 0x141CE5318};
    static const uintptr_t kSlots[] = {0x18, 0x28};
    for (const uintptr_t vt : kVts) {
        if (g_titleMode) break;  // title mode: the loading screen and title screen are the game's own
        for (const uintptr_t slot : kSlots) {
            auto* p = reinterpret_cast<uintptr_t*>(GameAddr(vt + slot));
            const uintptr_t expect = GameAddr(slot == 0x18 ? 0x140115660 : 0x1401157D0);
            const uintptr_t stub = reinterpret_cast<uintptr_t>(&NoOpWrapperCall);
            const bool match = *p == expect || *p == stub || (slot == 0x28);  // +0x28 may already be stubbed (bootvideo)
            ok &= match && WriteCode(reinterpret_cast<uintptr_t>(p), &stub, sizeof(stub));
        }
    }
    if (!g_titleMode) Log("gameflow: loading/title screen movies disabled");
    // 5. No pause freeze (see OurRun): the render thread's "paused" tests use the MP rule too
    //    (state == 4 && (!net+0x1634 || DAT_142b0ebc8)) - make each one take its "not paused" branch (JNZ -> JMP).
    if (g_noPauseFreeze) {
        struct Site { uintptr_t at; uint8_t disp; const char* what; };
        static const Site kSites[] = {
            {0x1405C8A46, 0x19, "render: dt-scaled effects"},          // FUN_1405c8720
            {0x1405CBD8E, 0x1E, "render: scene paused flag"},          // FUN_1405cbad0
            {0x1405CC44F, 0x1C, "render: scene paused flag (2)"},      // FUN_1405cbfd0
        };
        int patched = 0;
        for (const Site& s : kSites) {
            const uint8_t expect[2] = {0x75, s.disp};
            const uint8_t jmp[2] = {0xEB, s.disp};
            const auto* cur = reinterpret_cast<const uint8_t*>(GameAddr(s.at));
            if (std::memcmp(cur, expect, 2) == 0 && WriteCode(GameAddr(s.at), jmp, 2)) ++patched;
            else Log("gameflow: no-pause-freeze patch %s at %llx: bytes mismatch", s.what, static_cast<unsigned long long>(s.at));
        }
        Log("gameflow: world keeps running in the pause menu (%d/3 render patches)", patched);
    }
    // 6. Sign-in race at boot (see UiAlertUpdateHook).
    g_uiAlertOrig = reinterpret_cast<void (*)(uintptr_t, float)>(
        DetourPic(GameAddr(0x140330A60), kUiAlertPrologue, sizeof(kUiAlertPrologue), reinterpret_cast<void*>(&UiAlertUpdateHook)));
    Log("gameflow: UI alert update %s", g_uiAlertOrig ? "held during boot until signed in" : "hook FAILED (prologue mismatch)");
    g_installed = ok;
}

}  // namespace rdmp
