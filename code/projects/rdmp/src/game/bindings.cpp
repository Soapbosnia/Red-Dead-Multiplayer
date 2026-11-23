// The game's key bindings (RDR.exe build 0x673783F3; re/NOTES.md "Key bindings").
//
// Manager *(0x142AAC8D0); +0x98 a std::map<std::string, action> of every game action. Node: +0x19 is-nil, +0x20 key,
// value: +0x48 rebindable (saved to gabindings.xml), +0x50 std::string action name ("@FOOT.JUMP"), +0x80 primary
// {i32 type, i32 input}, +0x88 secondary. Types: rage's None / KeyboardButton (input = DirectInput scan code) /
// MouseButton / MouseMovement / MouseWheel.
// +0xE8 is a second map of the same kind: the working copy the game's keybind menu edits. Closing that menu commits it
// with FUN_140138A70(manager) (live = working, then FUN_1401388B0 rebuilds the input mapping from it) and saves with
// FUN_140138510(manager) (FUN_140E49A10). The live table alone is not read by the input system, so a rebind does the
// same: working = live (FUN_14013AF90 is the map assignment), edit the working copy, commit. It is not saved to the
// game's gabindings.xml (that is the single-player file, and the game's save fails here anyway): the menu keeps the
// player's multiplayer binds in rdmp.ini [game_binds] and applies them over the game's at every launch (MTA-style).
#include "bindings.h"

#include <windows.h>

#include <cstring>

#include "log.h"
#include "memcheck.h"
#include "patch.h"

namespace rdmp {
namespace {

constexpr uintptr_t kManager = 0x142AAC8D0;
constexpr uintptr_t kAssign  = 0x14013AF90;  // std::map assignment (dst, src)
constexpr uintptr_t kCommit  = 0x140138A70;
constexpr uintptr_t kLive    = 0x98;
constexpr uintptr_t kWorking = 0xE8;

// MSVC std::string: 16-byte inline buffer or a pointer, size at +0x10, capacity at +0x18.
const char* StdString(uintptr_t s) {
    const size_t cap = *reinterpret_cast<const size_t*>(s + 0x18);
    return cap >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
}

uintptr_t Manager() {
    const uintptr_t m = *reinterpret_cast<const uintptr_t*>(GameAddr(kManager));
    return m && IsReadable(m + kWorking, 0x10) ? m : 0;
}

// In-order walk of one of the manager's maps; fn(node) returns false to stop.
template <typename Fn>
void ForEachAction(uintptr_t map, Fn&& fn) {
    const uintptr_t head = *reinterpret_cast<const uintptr_t*>(map);
    if (!head || !IsReadable(head, 0x20)) return;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(head);  // leftmost
    int guard = 0;
    while (node && node != head && guard++ < 4096) {
        if (!IsReadable(node, 0x90) || *reinterpret_cast<const uint8_t*>(node + 0x19)) return;
        if (!fn(node)) return;
        // successor
        uintptr_t right = *reinterpret_cast<const uintptr_t*>(node + 0x10);
        if (!*reinterpret_cast<const uint8_t*>(right + 0x19)) {
            node = right;
            while (!*reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(node) + 0x19)) {
                node = *reinterpret_cast<const uintptr_t*>(node);
            }
        } else {
            uintptr_t parent = *reinterpret_cast<const uintptr_t*>(node + 0x8);
            while (!*reinterpret_cast<const uint8_t*>(parent + 0x19) && node == *reinterpret_cast<const uintptr_t*>(parent + 0x10)) {
                node   = parent;
                parent = *reinterpret_cast<const uintptr_t*>(node + 0x8);
            }
            node = parent;
        }
    }
}

}  // namespace

int ReadGameBindings(fw::GameBinding* out, int max) {
    const uintptr_t manager = Manager();
    if (!manager || !out) return 0;
    int n = 0;
    static bool logged = false;
    ForEachAction(manager + kLive, [&](uintptr_t node) {
        if (!*reinterpret_cast<const uint8_t*>(node + 0x48)) return true;  // not a player binding
        if (n >= max) return false;
        fw::GameBinding& b = out[n++];
        b = {};
        strncpy_s(b.action, StdString(node + 0x50), _TRUNCATE);
        for (int slot = 0; slot < 2; ++slot) {
            b.type[slot]  = *reinterpret_cast<const int32_t*>(node + 0x80 + slot * 8);
            b.input[slot] = *reinterpret_cast<const int32_t*>(node + 0x84 + slot * 8);
        }
        if (!logged) Log("bindings: %s = (%d, %d) / (%d, %d)", b.action, b.type[0], b.input[0], b.type[1], b.input[1]);
        return true;
    });
    logged = true;
    return n;
}

bool SetGameBinding(const char* action, int slot, int32_t type, int32_t input) {
    const uintptr_t manager = Manager();
    if (!manager || !action || slot < 0 || slot > 1) return false;
    reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(GameAddr(kAssign))(manager + kWorking, manager + kLive);
    bool found = false;
    ForEachAction(manager + kWorking, [&](uintptr_t node) {
        if (!*reinterpret_cast<const uint8_t*>(node + 0x48) || std::strcmp(StdString(node + 0x50), action) != 0) return true;
        *reinterpret_cast<int32_t*>(node + 0x80 + slot * 8) = type;
        *reinterpret_cast<int32_t*>(node + 0x84 + slot * 8) = input;
        found = true;
        return false;
    });
    if (!found) return false;
    reinterpret_cast<void (*)(uintptr_t)>(GameAddr(kCommit))(manager);
    Log("bindings: %s slot %d = (%d, %d), committed", action, slot, type, input);
    return true;
}

}  // namespace rdmp
