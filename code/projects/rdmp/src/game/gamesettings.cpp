// The game's own settings, through its own code: see gamesettings.h.
//
// RE (re/NOTES.md "Game settings"): the options menu object ("OptionsLayer", rdrOptionsLayer) keeps a
// rage::LoadedOptions at +0x98. Its SyncOptionsFromGame (FUN_140625f80) fills that from the live game, its
// SyncOptionsToGame (FUN_140627370) applies it, and closing the menu saves it (FUN_14061f410 = options.xml through the
// parser). The two volumes are not in ToGame: they are entries of the audio category table (*(0x142AE3380), stride
// 0x14, index *(u16*)(category+0x28); categories SFX *(0x142AE2E10), music *(0x142AE2E18)). Graphics live in one
// rage::GraphicsOptions at 0x1422CA340; the graphics menu's ApplyCurrentGraphicsOptions (FUN_1405d7260) hands a
// pending copy to every registered setting handler (std::map at *(0x142B67370), handler = node+0x28, vfunc +0x10)
// and saves graphicsOptions.xml (FUN_140158be0). Here a buffer shaped like the menu object stands in for the menu.
#include "gamesettings.h"

#include <windows.h>

#include <cstring>

#include "log.h"
#include "memcheck.h"
#include "patch.h"
#include "shared/game_options.h"

namespace rdmp {
namespace {

constexpr uintptr_t kFromGame = 0x140625F80;
constexpr uintptr_t kToGame = 0x140627370;
constexpr uintptr_t kSaveOptions = 0x14061F410;
constexpr uintptr_t kSaveGraphics = 0x140158BE0;
constexpr uintptr_t kLiveGraphics = 0x1422CA340;
constexpr uintptr_t kGraphicsHandlers = 0x142B67370;
constexpr uintptr_t kAudioCategoryTable = 0x142AE3380;
constexpr uintptr_t kSfxCategory = 0x142AE2E10;
constexpr uintptr_t kMusicCategory = 0x142AE2E18;
constexpr size_t kLoadedAt = 0x98;  // LoadedOptions inside the menu object

uintptr_t Ptr(uintptr_t a) { return IsReadable(a, 8) ? *reinterpret_cast<uintptr_t*>(a) : 0; }

// A stand-in for the options menu object: zeroed, the two "no override" indices its sync code checks set to -1.
alignas(16) uint8_t g_layer[0x200];
void ResetLayer() {
    std::memset(g_layer, 0, sizeof(g_layer));
    *reinterpret_cast<int32_t*>(g_layer + 0x110) = -1;
    *reinterpret_cast<int32_t*>(g_layer + 0x114) = -1;
}

float* VolumeSlot(uintptr_t categoryGlobal) {
    const uintptr_t table = Ptr(GameAddr(kAudioCategoryTable));
    const uintptr_t category = Ptr(GameAddr(categoryGlobal));
    if (!table || !category || !IsReadable(category + 0x28, 2)) return nullptr;
    const uintptr_t slot = table + static_cast<uintptr_t>(*reinterpret_cast<uint16_t*>(category + 0x28)) * 0x14;
    return IsReadable(slot, 4) ? reinterpret_cast<float*>(slot) : nullptr;
}

bool IsNil(uintptr_t node) { return !node || !IsReadable(node + 0x19, 1) || *reinterpret_cast<uint8_t*>(node + 0x19) != 0; }

// Every registered graphics setting handler applies its part of `pending` (the graphics menu's apply loop).
int RunGraphicsHandlers(uint8_t* pending) {
    uintptr_t node = Ptr(Ptr(GameAddr(kGraphicsHandlers)));  // leftmost node of the std::map
    int n = 0;
    using HandlerFn = void (*)(uintptr_t handler, uint8_t* pending);
    while (!IsNil(node) && n < 256) {
        const uintptr_t handler = Ptr(node + 0x28);
        if (handler && Ptr(handler)) {
            reinterpret_cast<HandlerFn>(Ptr(Ptr(handler) + 0x10))(handler, pending);
            ++n;
        }
        // in-order successor
        const uintptr_t right = Ptr(node + 0x10);
        if (!IsNil(right)) {
            node = right;
            while (!IsNil(Ptr(node))) node = Ptr(node);
        } else {
            uintptr_t parent = Ptr(node + 0x08);
            while (!IsNil(parent) && node == Ptr(parent + 0x10)) {
                node = parent;
                parent = Ptr(parent + 0x08);
            }
            node = parent;
        }
    }
    return n;
}

}  // namespace

bool ReadGameOptions(uint8_t* loaded, uint8_t* graphics) {
    if (loaded) {
        ResetLayer();
        reinterpret_cast<void (*)(uint8_t*)>(GameAddr(kFromGame))(g_layer);
        std::memcpy(loaded, g_layer + kLoadedAt, opt::kLoadedSize);
    }
    if (graphics) {
        const uintptr_t live = GameAddr(kLiveGraphics);
        if (!IsReadable(live, opt::kGraphicsSize)) return false;
        std::memcpy(graphics, reinterpret_cast<void*>(live), opt::kGraphicsSize);
    }
    return true;
}

bool ApplyGameOptions(const uint8_t* loaded, const uint8_t* graphics) {
    if (loaded) {
        ResetLayer();
        reinterpret_cast<void (*)(uint8_t*)>(GameAddr(kFromGame))(g_layer);  // fields ToGame reads but we do not own
        std::memcpy(g_layer + kLoadedAt, loaded, opt::kLoadedSize);
        reinterpret_cast<void (*)(uint8_t*)>(GameAddr(kToGame))(g_layer);
        float sfx = 0.0f, music = 0.0f;
        std::memcpy(&sfx, loaded + 0x54, 4);
        std::memcpy(&music, loaded + 0x58, 4);
        if (float* s = VolumeSlot(kSfxCategory)) *s = sfx;
        if (float* m = VolumeSlot(kMusicCategory)) *m = music;
        reinterpret_cast<void (*)(uint8_t*)>(GameAddr(kSaveOptions))(g_layer + kLoadedAt);
        Log("game settings: options applied and saved (options.xml)");
    }
    if (graphics) {
        alignas(16) uint8_t pending[0x80] = {};
        std::memcpy(pending, graphics, opt::kGraphicsSize);
        const int handlers = RunGraphicsHandlers(pending);
        const uintptr_t live = GameAddr(kLiveGraphics);
        if (IsReadable(live, opt::kGraphicsSize)) std::memcpy(reinterpret_cast<void*>(live), pending, opt::kGraphicsSize);
        reinterpret_cast<void (*)()>(GameAddr(kSaveGraphics))();
        Log("game settings: graphics applied by %d handler(s) and saved (graphicsOptions.xml)", handlers);
    }
    return true;
}

}  // namespace rdmp
