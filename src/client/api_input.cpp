/*
 * CLIENT input:
 *   isKeyDown(key) -> bool         key names: "a".."z", "0".."9", "f1".."f12", "num_0".."num_9", "enter", "space",
 *                                  "escape", "tab", "backspace", "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt",
 *                                  "arrow_u/d/l/r", "insert", "delete", "home", "end", "pgup", "pgdn", "mouse1..3"
 *   getTickCount() -> ms
 * Events: clientKey(key, down), clientClick(button, state, x, y) (api_core.cpp).
 */

#include <utils/safe_win32.h>

#include "client/api.h"

#include <cctype>
#include <cstdio>

namespace RDMP::ClientApi {
    namespace {
        using namespace ScriptApi;

    std::vector<KeyName> BuildKeys() {
        std::vector<KeyName> keys = {
            {VK_RETURN, "enter"}, {VK_SPACE, "space"}, {VK_ESCAPE, "escape"}, {VK_TAB, "tab"}, {VK_BACK, "backspace"},
            {VK_LSHIFT, "lshift"}, {VK_RSHIFT, "rshift"}, {VK_LCONTROL, "lctrl"}, {VK_RCONTROL, "rctrl"},
            {VK_LMENU, "lalt"}, {VK_RMENU, "ralt"}, {VK_UP, "arrow_u"}, {VK_DOWN, "arrow_d"}, {VK_LEFT, "arrow_l"},
            {VK_RIGHT, "arrow_r"}, {VK_INSERT, "insert"}, {VK_DELETE, "delete"}, {VK_HOME, "home"}, {VK_END, "end"},
            {VK_PRIOR, "pgup"}, {VK_NEXT, "pgdn"}, {VK_LBUTTON, "mouse1"}, {VK_RBUTTON, "mouse2"}, {VK_MBUTTON, "mouse3"},
        };
        static char letters[26][2], digits[10][2], fkeys[12][4], numpad[10][6];
        for (int i = 0; i < 26; ++i) {
            letters[i][0] = static_cast<char>('a' + i);
            keys.push_back({'A' + i, letters[i]});
        }
        for (int i = 0; i < 10; ++i) {
            digits[i][0] = static_cast<char>('0' + i);
            keys.push_back({'0' + i, digits[i]});
            std::snprintf(numpad[i], sizeof(numpad[i]), "num_%d", i);
            keys.push_back({VK_NUMPAD0 + i, numpad[i]});
        }
        for (int i = 0; i < 12; ++i) {
            std::snprintf(fkeys[i], sizeof(fkeys[i]), "f%d", i + 1);
            keys.push_back({VK_F1 + i, fkeys[i]});
        }
        return keys;
    }

    void IsKeyDownFn(const Args &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString()) {
            Throw(isolate, "isKeyDown(key)");
            return;
        }
        std::string name = Str(isolate, args[0]);
        for (auto &c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        bool down = false;
        if (Host() && Host()->game_has_focus()) {
            for (const auto &k : Keys()) {
                if (name == k.name) {
                    down = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
                    break;
                }
            }
        }
        args.GetReturnValue().Set(down);
    }


        void GetTickCountFn(const Args &args) {
            args.GetReturnValue().Set(static_cast<double>(GetTickCount64()));
        }
    } // namespace

    const std::vector<KeyName> &Keys() {
        static const std::vector<KeyName> keys = BuildKeys();
        return keys;
    }

    void RegisterInputApi(const ScriptApi::Publisher &publish) {
        publish("isKeyDown", &IsKeyDownFn);
        publish("getTickCount", &GetTickCountFn);
    }
} // namespace RDMP::ClientApi
