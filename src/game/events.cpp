// UI event logger.
//
// RE (re/NOTES.md): the boot/menu flow advances on named UI events ("startPadSelected",
// "fileStartNewGame", "loadStart", ...). The game posts them with
//   (*(ui->vtable + 0x390))(ui, "eventName", 0, 10, 1, 1, 0)
// where ui = *(void**)0x142aac8b8 (UI manager). We swap that vtable slot for a logging wrapper.
#include "events.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "sandbox.h"
#include "watch.h"

namespace rdmp {

bool g_watchOnLoad = false;

namespace {

constexpr uintptr_t kUiManagerPtr = 0x142AAC8B8;   // global: UI manager object pointer
constexpr size_t kPostEventSlot = 0x390 / 8;       // vtable slot of the post-event method

using PostEventFn = uintptr_t (*)(void* ui, const char* name, uintptr_t a3, uintptr_t a4, uintptr_t a5,
                                  uintptr_t a6, uintptr_t a7);
std::atomic<PostEventFn> g_origPostEvent{nullptr};

void* UiManager() { return *reinterpret_cast<void**>(GameAddr(kUiManagerPtr)); }

// Copies the event name safely (the pointer comes from game code).
void SafeName(const char* name, char* out, size_t outLen) {
    out[0] = '\0';
    if (!IsReadable(reinterpret_cast<uintptr_t>(name), 1)) {
        strcpy_s(out, outLen, "<unreadable>");
        return;
    }
    size_t i = 0;
    for (; i + 1 < outLen && name[i]; ++i) {
        if (((reinterpret_cast<uintptr_t>(name) + i) & 0xFFF) == 0 &&
            !IsReadable(reinterpret_cast<uintptr_t>(name) + i, 1)) break;
        out[i] = name[i];
    }
    out[i] = '\0';
}

uintptr_t PostEventHook(void* ui, const char* name, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                        uintptr_t a7) {
    char buf[128];
    SafeName(name, buf, sizeof(buf));
    Log("ui event: \"%s\" (%llu, %llu, %llu, %llu, %llu)", buf, a3, a4, a5, a6, a7);
    // [debug] watch_player_handle=1: catch who assigns the player's actor handle during the world load.
    if (g_watchOnLoad && strcmp(buf, "loadStart") == 0) ArmPlayerHandleWatch();
    const uintptr_t r = g_origPostEvent.load()(ui, name, a3, a4, a5, a6, a7);
    SandboxOnUiEvent(buf);  // after delivery, on the posting (main) thread
    return r;
}

DWORD WINAPI WatcherThread(LPVOID) {
    // The UI manager is created during boot; wait for it (up to ~2 minutes).
    void* ui = nullptr;
    for (int i = 0; i < 1200 && !(ui = UiManager()); ++i) Sleep(100);
    if (!ui) {
        Log("events: UI manager never appeared, logger not installed");
        return 0;
    }
    auto** vtable = *reinterpret_cast<void***>(ui);
    void** slot = &vtable[kPostEventSlot];
    g_origPostEvent = reinterpret_cast<PostEventFn>(*slot);
    void* hook = reinterpret_cast<void*>(&PostEventHook);
    if (!WriteCode(reinterpret_cast<uintptr_t>(slot), &hook, sizeof(hook))) {
        Log("events: failed to patch vtable slot (error %lu)", GetLastError());
        return 0;
    }
    Log("events: logger installed (ui=%p vtable=%p original post=%p)", ui, vtable,
        reinterpret_cast<void*>(g_origPostEvent.load()));
    return 0;
}

}  // namespace

void StartEventLogger() {
    HANDLE t = CreateThread(nullptr, 0, &WatcherThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

bool PostUiEvent(const char* name) {
    void* ui = UiManager();
    PostEventFn post = g_origPostEvent.load();
    if (!ui || !post) return false;
    post(ui, name, 0, 10, 1, 1, 0);  // same arguments the game uses
    return true;
}

}  // namespace rdmp
