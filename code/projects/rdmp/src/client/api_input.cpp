/*
 * CLIENT input:
 *   isKeyDown(key) -> bool         key names: "a".."z", "0".."9", "f1".."f12", "num_0".."num_9", "enter", "space",
 *                                  "escape", "tab", "backspace", "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt",
 *                                  "arrow_u/d/l/r", "insert", "delete", "home", "end", "pgup", "pgdn", "mouse1..3"
 *   getTickCount() -> ms
 *   bindKey(key, "down" | "up" | "both", handler(key, state) [, description]) -> true
 *                                  MTA-style: a bind with a description is listed in the player's Binds settings,
 *                                  where the player can move it to another key (script_binds.h)
 *   unbindKey(key [, state [, handler]]) -> bool   this resource's binds on that key
 * Events: clientKey(key, down), clientClick(button, state, x, y) (api_core.cpp).
 */

#include <utils/safe_win32.h>

#include "client/api.h"
#include "client/fw_client.h"
#include "client/script_binds.h"

#include <scripting/engine_scope.h>
#include <scripting/resource/resource_manager.h>
#include <utils/path.h>

#include <algorithm>
#include <memory>

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

    // ---- bindKey / unbindKey (script_binds.h) ----
    struct Bind {
        std::string resource;
        std::string key;   // the script's key
        std::string state; // "down", "up", "both"
        std::string description;
        v8::Global<v8::Function> handler;
    };
    std::vector<std::unique_ptr<Bind>> g_binds;
    bool g_bindsHooked = false;

    std::wstring IniPath() {
        return std::wstring(Framework::Utils::GetModuleDirW()) + L"\\rdmp.ini";
    }

    std::wstring Wide(const std::string &s) {
        wchar_t w[512];
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, 512);
        return w;
    }

    // The player's key for a described bind, "" = none chosen.
    std::string PlayerKey(const std::string &resource, const std::string &description) {
        if (description.empty()) {
            return {};
        }
        wchar_t w[64];
        GetPrivateProfileStringW(L"resource_binds", Wide(resource + "/" + description).c_str(), L"", w, 64, IniPath().c_str());
        char out[128];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out), nullptr, nullptr);
        return out;
    }

    std::string Lower(std::string s) {
        for (auto &c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string CallingResource(v8::Isolate *isolate) {
        auto *client = Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *rm     = module ? module->GetResourceManager() : nullptr;
        auto *res    = rm ? rm->GetCurrentResourceWithStackFallback(isolate) : nullptr;
        return res ? res->GetName() : std::string("unknown");
    }

    void HookResourceStops() {
        auto *client = Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *rm     = module ? module->GetResourceManager() : nullptr;
        if (g_bindsHooked || !rm) {
            return;
        }
        g_bindsHooked = true;
        rm->AddOnResourceStopped([](const std::string &resource) {
            g_binds.erase(std::remove_if(g_binds.begin(), g_binds.end(), [&](const std::unique_ptr<Bind> &b) { return b->resource == resource; }),
                          g_binds.end());
        });
    }

    // bindKey(key, "down" | "up" | "both", handler(key, state) [, description])
    void BindKeyFn(const Args &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString() || !Has(args, 2) || !args[2]->IsFunction()) {
            Throw(isolate, "bindKey(key, \"down\" | \"up\" | \"both\", handler(key, state) [, description])");
            return;
        }
        const std::string key   = Lower(Str(isolate, args[0]));
        const std::string state = Has(args, 1) && args[1]->IsString() ? Lower(Str(isolate, args[1])) : "down";
        if (RDMP::ScriptBinds::KeyCode(key) == 0 || (state != "down" && state != "up" && state != "both")) {
            Throw(isolate, "bindKey: unknown key or state (\"" + key + "\", \"" + state + "\")");
            return;
        }
        HookResourceStops();
        auto bind         = std::make_unique<Bind>();
        bind->resource    = CallingResource(isolate);
        bind->key         = key;
        bind->state       = state;
        bind->description = Has(args, 3) && args[3]->IsString() ? Str(isolate, args[3]) : std::string();
        bind->handler.Reset(isolate, args[2].As<v8::Function>());
        g_binds.push_back(std::move(bind));
        args.GetReturnValue().Set(true);
    }

    // unbindKey(key [, state [, handler]]): this resource's binds on that key.
    void UnbindKeyFn(const Args &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString()) {
            Throw(isolate, "unbindKey(key [, state [, handler]])");
            return;
        }
        const std::string key      = Lower(Str(isolate, args[0]));
        const std::string state    = Has(args, 1) && args[1]->IsString() ? Lower(Str(isolate, args[1])) : std::string();
        const std::string resource = CallingResource(isolate);
        v8::Local<v8::Function> fn;
        if (Has(args, 2) && args[2]->IsFunction()) {
            fn = args[2].As<v8::Function>();
        }
        const size_t before = g_binds.size();
        g_binds.erase(std::remove_if(g_binds.begin(), g_binds.end(),
                                     [&](const std::unique_ptr<Bind> &b) {
                                         return b->resource == resource && b->key == key && (state.empty() || b->state == state) &&
                                                (fn.IsEmpty() || b->handler.Get(isolate) == fn);
                                     }),
                      g_binds.end());
        args.GetReturnValue().Set(g_binds.size() != before);
    }
    } // namespace

    const std::vector<KeyName> &Keys() {
        static const std::vector<KeyName> keys = BuildKeys();
        return keys;
    }

    void RegisterInputApi(const ScriptApi::Publisher &publish) {
        publish("isKeyDown", &IsKeyDownFn);
        publish("getTickCount", &GetTickCountFn);
        publish("bindKey", &BindKeyFn);
        publish("unbindKey", &UnbindKeyFn);
    }

    // A key went down or up: the binds on it run (the player's key for described binds, the script's otherwise).
    void DispatchBinds(const char *key, bool down) {
        if (g_binds.empty()) {
            return;
        }
        auto *client = Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine) {
            return;
        }
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return;
        }
        auto *isolate = scope.Isolate();
        // Handlers may bind / unbind: run over a snapshot of the matching ones.
        std::vector<Bind *> hits;
        for (const auto &b : g_binds) {
            const std::string chosen = PlayerKey(b->resource, b->description);
            const std::string &on    = chosen.empty() ? b->key : chosen;
            if (on == key && (b->state == "both" || (b->state == "down") == down)) {
                hits.push_back(b.get());
            }
        }
        for (Bind *b : hits) {
            if (std::none_of(g_binds.begin(), g_binds.end(), [&](const std::unique_ptr<Bind> &x) { return x.get() == b; })) {
                continue; // unbound by an earlier handler
            }
            v8::HandleScope handles(isolate);
            v8::Local<v8::Function> fn = b->handler.Get(isolate);
            v8::Local<v8::Context> ctx = fn->GetCreationContextChecked(isolate);
            v8::Context::Scope inContext(ctx);
            v8::TryCatch tryCatch(isolate);
            v8::Local<v8::Value> argv[2] = {ScriptApi::JsString(isolate, b->key), ScriptApi::JsString(isolate, down ? "down" : "up")};
            if (fn->Call(ctx, v8::Undefined(isolate), 2, argv).IsEmpty() && tryCatch.HasCaught()) {
                v8::String::Utf8Value message(isolate, tryCatch.Exception());
                RDMP::Log()->error("bindKey handler ({}): {}", b->resource, *message ? *message : "error");
            }
        }
    }
} // namespace RDMP::ClientApi

namespace RDMP::ScriptBinds {
    std::vector<Entry> List() {
        std::vector<Entry> out;
        for (const auto &b : ClientApi::g_binds) {
            if (b->description.empty()) {
                continue;
            }
            const bool seen = std::any_of(out.begin(), out.end(), [&](const Entry &e) { return e.resource == b->resource && e.description == b->description; });
            if (seen) {
                continue;
            }
            const std::string chosen = ClientApi::PlayerKey(b->resource, b->description);
            out.push_back({b->resource, b->description, chosen.empty() ? b->key : chosen, b->key});
        }
        return out;
    }

    void SetKey(const std::string &resource, const std::string &description, const std::string &key) {
        const std::wstring ini = ClientApi::IniPath();
        WritePrivateProfileStringW(L"resource_binds", ClientApi::Wide(resource + "/" + description).c_str(), key.empty() ? nullptr : ClientApi::Wide(key).c_str(),
                                   ini.c_str());
    }

    int KeyCode(const std::string &name) {
        for (const auto &k : ClientApi::Keys()) {
            if (name == k.name) {
                return k.vk;
            }
        }
        return 0;
    }

    std::string KeyName(int vk) {
        for (const auto &k : ClientApi::Keys()) {
            if (k.vk == vk) {
                return k.name;
            }
        }
        return {};
    }
} // namespace RDMP::ScriptBinds

