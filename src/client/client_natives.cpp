/*
 * RDMP client script natives. Client scripts run on the game's main thread (rdmp.dll pumps the framework client
 * from its frame callback), so these call straight into the game through the Host table.
 *
 *   getLocalPlayer()                         -> element | null
 *   getLocalActor()                          -> the local player's actor handle (for invokeNative), 0 = none
 *   getElementPosition(element)              -> {x, y, z}        (local player: live game position)
 *   setElementPosition(localPlayer, x, y, z)                     (the local player only)
 *   getElementHealth(element) / setElementHealth(localPlayer, hp)
 *   getPlayerName(player), getElementType(element), isElement(element), getElementsByType("player"|"blip")
 *   fadeCamera(fadeIn, ms), setCameraTarget(), toggleAllControls(enabled)
 *   outputChatBox(text)                      a line on this player's screen
 *   isKeyDown(key), getTickCount()
 *   invokeNative(nameOrHash, types, ...args) any game native, e.g. invokeNative("SET_WEATHER", "if", 3, 1.0)
 *       types: i int, u uint, f float, b bool, s string, v Vector3 in ({x,y,z} or [x,y,z]), V Vector3 out;
 *       ">x" at the end sets the return type (i, u, f, b, n = none; default i). With V args the result is
 *       {result, vectors: [{x,y,z}, ...]}.
 *   BlipIcon.*                               icon ids by name
 * Events: "clientKey" (key, down) while the game has focus, "clientRender" (ms since the last frame).
 */

#include <utils/safe_win32.h>

#include "client/client_natives.h"
#include "client/fw_client.h"
#include "shared/blip_icons.h"

#include <scripting/builtins/entity.h>
#include <scripting/engine_scope.h>
#include <scripting/resource/resource_manager.h>
#include <v8pp/convert.hpp>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
    using rdmp::fw::Host;
    namespace Builtins = Framework::Scripting::Builtins;

    RDMP::FwClient *g_client = nullptr;
    const Host *g_host       = nullptr;

    uint64_t LocalId() {
        auto *me = g_client ? g_client->Owned() : nullptr;
        return me ? static_cast<uint64_t>(me->GetNetworkID()) : 0;
    }

    void Throw(v8::Isolate *isolate, const std::string &usage) {
        isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8(isolate, usage.c_str()).ToLocalChecked()));
    }

    bool Has(const v8::FunctionCallbackInfo<v8::Value> &args, int i) {
        return args.Length() > i && !args[i]->IsUndefined() && !args[i]->IsNull();
    }

    double Num(const v8::FunctionCallbackInfo<v8::Value> &args, int i, double fallback) {
        if (!Has(args, i) || !args[i]->IsNumber()) {
            return fallback;
        }
        return args[i].As<v8::Number>()->Value();
    }

    std::string Str(v8::Isolate *isolate, v8::Local<v8::Value> v) {
        return *v8::String::Utf8Value(isolate, v);
    }

    uint64_t ElementId(const v8::FunctionCallbackInfo<v8::Value> &args, int i) {
        if (!Has(args, i) || !args[i]->IsObject()) {
            return 0;
        }
        auto *handle = v8pp::from_v8<Builtins::Entity *>(args.GetIsolate(), args[i]);
        return handle ? handle->GetId() : 0;
    }

    v8::Local<v8::Object> Vec(v8::Isolate *isolate, float x, float y, float z) {
        auto context = isolate->GetCurrentContext();
        auto o       = v8::Object::New(isolate);
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "x"), v8::Number::New(isolate, x)).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "y"), v8::Number::New(isolate, y)).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "z"), v8::Number::New(isolate, z)).Check();
        return o;
    }

    bool LocalPlayer(float pos[3], float *heading = nullptr, float *health = nullptr, uint32_t *actor = nullptr) {
        return g_host && g_host->get_local_player && g_host->get_local_player(pos, heading, health, actor);
    }

    v8::Local<v8::Value> Wrap(v8::Isolate *isolate, uint64_t id) {
        if (!id || !g_client) {
            return v8::Null(isolate);
        }
        return g_client->WrapScriptEntity(isolate, id);
    }

    // ---- elements ----
    void GetLocalPlayerFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        args.GetReturnValue().Set(Wrap(args.GetIsolate(), LocalId()));
    }

    void GetLocalActorFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        float pos[3];
        uint32_t actor = 0;
        LocalPlayer(pos, nullptr, nullptr, &actor);
        args.GetReturnValue().Set(actor);
    }

    void GetElementPositionFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        float p[3]        = {};
        if (id && id == LocalId() && LocalPlayer(p)) {
            args.GetReturnValue().Set(Vec(isolate, p[0], p[1], p[2]));
            return;
        }
        if (auto *player = g_client ? g_client->Entity<RDMP::Shared::Player>(id) : nullptr) {
            args.GetReturnValue().Set(Vec(isolate, player->position.x, player->position.y, player->position.z));
            return;
        }
        if (auto *blip = g_client ? g_client->Entity<RDMP::Shared::Blip>(id) : nullptr) {
            args.GetReturnValue().Set(Vec(isolate, blip->position.x, blip->position.y, blip->position.z));
            return;
        }
        Throw(isolate, "getElementPosition(element) -> {x, y, z}");
    }

    void SetElementPositionFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        if (ElementId(args, 0) != LocalId() || args.Length() < 4 || !g_host) {
            Throw(isolate, "setElementPosition(localPlayer, x, y, z)");
            return;
        }
        float p[3] = {}, heading = 0.0f;
        LocalPlayer(p, &heading);
        const float to[3] = {static_cast<float>(Num(args, 1, p[0])), static_cast<float>(Num(args, 2, p[1])), static_cast<float>(Num(args, 3, p[2]))};
        g_host->teleport_local(to, heading);
        args.GetReturnValue().Set(true);
    }

    void GetElementHealthFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t id = ElementId(args, 0);
        float p[3] = {}, hp = 0.0f;
        if (id && id == LocalId() && LocalPlayer(p, nullptr, &hp)) {
            args.GetReturnValue().Set(static_cast<double>(hp));
            return;
        }
        auto *player = g_client ? g_client->Entity<RDMP::Shared::Player>(id) : nullptr;
        args.GetReturnValue().Set(player ? static_cast<int>(player->localHealth) : 0);
    }

    void SetElementHealthFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        if (ElementId(args, 0) != LocalId() || !g_host) {
            Throw(args.GetIsolate(), "setElementHealth(localPlayer, health)");
            return;
        }
        g_host->set_health(static_cast<float>(Num(args, 1, 100.0)));
        args.GetReturnValue().Set(true);
    }

    void GetPlayerNameFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *player = g_client ? g_client->Entity<RDMP::Shared::Player>(ElementId(args, 0)) : nullptr;
        const std::string name = player ? player->nickname : std::string();
        args.GetReturnValue().Set(v8::String::NewFromUtf8(args.GetIsolate(), name.c_str()).ToLocalChecked());
    }

    void GetElementTypeFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        if (g_client && g_client->Entity<RDMP::Shared::Player>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "player"));
        }
        else if (g_client && g_client->Entity<RDMP::Shared::Blip>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "blip"));
        }
        else {
            args.GetReturnValue().Set(false);
        }
    }

    void IsElementFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        const uint64_t id = ElementId(args, 0);
        const bool exists = g_client && id && (g_client->Entity<RDMP::Shared::Player>(id) || g_client->Entity<RDMP::Shared::Blip>(id));
        args.GetReturnValue().Set(exists);
    }

    void GetElementsByTypeFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString() || !g_client) {
            Throw(isolate, "getElementsByType(\"player\"|\"blip\") -> [elements]");
            return;
        }
        const std::string type = Str(isolate, args[0]);
        std::vector<uint64_t> ids;
        if (type == "player") {
            if (const uint64_t me = LocalId()) {
                ids.push_back(me);
            }
            g_client->ForEachRemote([&](RDMP::Shared::Player *p) { ids.push_back(p->GetNetworkID()); });
        }
        else if (type == "blip") {
            g_client->ForEachBlip([&](RDMP::Shared::Blip *b) { ids.push_back(b->GetNetworkID()); });
        }
        auto context = isolate->GetCurrentContext();
        auto result  = v8::Array::New(isolate, static_cast<int>(ids.size()));
        for (uint32_t i = 0; i < ids.size(); ++i) {
            result->Set(context, i, Wrap(isolate, ids[i])).Check();
        }
        args.GetReturnValue().Set(result);
    }

    // ---- camera, control, screen ----
    void FadeCameraFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        if (g_host) {
            g_host->fade(args.Length() > 0 && args[0]->BooleanValue(args.GetIsolate()) ? 1 : 0, static_cast<float>(Num(args, 1, 1000.0)));
        }
    }

    void SetCameraTargetFn(const v8::FunctionCallbackInfo<v8::Value> &) {
        if (g_host) {
            g_host->camera_to_player();
        }
    }

    void ToggleAllControlsFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        if (g_host) {
            g_host->set_control(args.Length() == 0 || args[0]->BooleanValue(args.GetIsolate()) ? 1 : 0);
        }
    }

    void OutputChatBoxFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        if (g_host && args.Length() > 0) {
            const std::string text = Str(args.GetIsolate(), args[0]);
            g_host->print_text(text.c_str(), static_cast<int>(Num(args, 1, 6000.0)));
        }
    }

    void GetTickCountFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        args.GetReturnValue().Set(static_cast<double>(GetTickCount64()));
    }

    // ---- keys ----
    struct KeyName {
        int vk;
        const char *name;
    };
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
    const std::vector<KeyName> &Keys() {
        static const std::vector<KeyName> keys = BuildKeys();
        return keys;
    }

    void IsKeyDownFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
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
        if (g_host && g_host->game_has_focus()) {
            for (const auto &k : Keys()) {
                if (name == k.name) {
                    down = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
                    break;
                }
            }
        }
        args.GetReturnValue().Set(down);
    }

    // ---- natives ----
    uint32_t Joaat(const std::string &name) {
        uint32_t h = 0;
        for (char ch : name) {
            h += static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(ch)));
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }

    bool ReadVector(v8::Isolate *isolate, v8::Local<v8::Value> v, std::array<float, 3> &out) {
        if (!v->IsObject()) {
            return false;
        }
        auto context = isolate->GetCurrentContext();
        auto o       = v.As<v8::Object>();
        const char *keys[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            v8::Local<v8::Value> c;
            if (v->IsArray()) {
                if (!o->Get(context, static_cast<uint32_t>(i)).ToLocal(&c)) {
                    return false;
                }
            }
            else if (!o->Get(context, v8::String::NewFromUtf8(isolate, keys[i]).ToLocalChecked()).ToLocal(&c)) {
                return false;
            }
            out[i] = c->IsNumber() ? static_cast<float>(c.As<v8::Number>()->Value()) : 0.0f;
        }
        return true;
    }

    void InvokeNativeFn(const v8::FunctionCallbackInfo<v8::Value> &args) {
        auto *isolate      = args.GetIsolate();
        const char *usage  = "invokeNative(nameOrHash, types, ...args): types i u f b s v V, \">x\" return type";
        if (args.Length() < 2 || !args[1]->IsString() || !g_host) {
            Throw(isolate, usage);
            return;
        }
        uint32_t hash = 0;
        if (args[0]->IsString()) {
            hash = Joaat(Str(isolate, args[0]));
        }
        else if (args[0]->IsNumber()) {
            hash = static_cast<uint32_t>(args[0].As<v8::Number>()->Value());
        }
        else {
            Throw(isolate, usage);
            return;
        }
        const std::string spec = Str(isolate, args[1]);
        const size_t arrow     = spec.find('>');
        const std::string in   = spec.substr(0, arrow);
        const char ret         = arrow != std::string::npos && arrow + 1 < spec.size() ? spec[arrow + 1] : 'i';
        if (static_cast<int>(in.size()) != args.Length() - 2 || in.size() > 24) {
            Throw(isolate, std::string(usage) + " (types and arguments differ in count)");
            return;
        }
        uint64_t slots[24] = {};
        std::vector<std::string> strings;
        strings.reserve(in.size());
        std::vector<std::array<float, 3>> vectors;
        vectors.reserve(in.size());
        std::vector<size_t> outVectors;
        for (size_t i = 0; i < in.size(); ++i) {
            v8::Local<v8::Value> v = args[static_cast<int>(i) + 2];
            uint32_t bits          = 0;
            switch (in[i]) {
            case 'i': bits = static_cast<uint32_t>(static_cast<int32_t>(v->IsNumber() ? v.As<v8::Number>()->Value() : 0)); slots[i] = bits; break;
            case 'u': slots[i] = static_cast<uint32_t>(v->IsNumber() ? v.As<v8::Number>()->Value() : 0); break;
            case 'b': slots[i] = v->BooleanValue(isolate) ? 1u : 0u; break;
            case 'f': {
                const float f = v->IsNumber() ? static_cast<float>(v.As<v8::Number>()->Value()) : 0.0f;
                std::memcpy(&bits, &f, 4);
                slots[i] = bits;
                break;
            }
            case 's':
                strings.push_back(Str(isolate, v));
                slots[i] = reinterpret_cast<uint64_t>(strings.back().c_str());
                break;
            case 'v':
            case 'V': {
                vectors.push_back({0.0f, 0.0f, 0.0f});
                if (in[i] == 'v' && !ReadVector(isolate, v, vectors.back())) {
                    Throw(isolate, "invokeNative: a 'v' argument must be {x, y, z} or [x, y, z]");
                    return;
                }
                if (in[i] == 'V') {
                    outVectors.push_back(vectors.size() - 1);
                }
                slots[i] = reinterpret_cast<uint64_t>(vectors.back().data());
                break;
            }
            default: Throw(isolate, usage); return;
            }
        }
        uint64_t result = 0;
        if (!g_host->invoke_native(hash, slots, static_cast<int>(in.size()), &result)) {
            Throw(isolate, "invokeNative: native not registered (wrong name?)");
            return;
        }
        v8::Local<v8::Value> value;
        const uint32_t low = static_cast<uint32_t>(result);
        switch (ret) {
        case 'u': value = v8::Number::New(isolate, low); break;
        case 'b': value = v8::Boolean::New(isolate, (low & 0xff) != 0); break;
        case 'f': {
            float f = 0.0f;
            std::memcpy(&f, &low, 4);
            value = v8::Number::New(isolate, f);
            break;
        }
        case 'n': value = v8::Undefined(isolate); break;
        default: value = v8::Integer::New(isolate, static_cast<int32_t>(low)); break;
        }
        if (outVectors.empty()) {
            args.GetReturnValue().Set(value);
            return;
        }
        auto context = isolate->GetCurrentContext();
        auto list    = v8::Array::New(isolate, static_cast<int>(outVectors.size()));
        for (uint32_t i = 0; i < outVectors.size(); ++i) {
            const auto &v = vectors[outVectors[i]];
            list->Set(context, i, Vec(isolate, v[0], v[1], v[2])).Check();
        }
        auto o = v8::Object::New(isolate);
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "result"), value).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "vectors"), list).Check();
        args.GetReturnValue().Set(o);
    }

    // ---- per-frame events ----
    void Emit(const char *name, std::vector<v8::Local<v8::Value>> (*makeArgs)(v8::Isolate *, const void *), const void *data) {
        auto *module = g_client ? g_client->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine || !module->GetResourceManager()) {
            static bool once = false;
            if (!once) RDMP::Log()->warn("client events: no scripting engine yet, '{}' dropped", name), once = true;
            return;
        }
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            static bool once = false;
            if (!once) RDMP::Log()->warn("client events: engine scope invalid, '{}' dropped", name), once = true;
            return;
        }
        // Sync: handlers run now, on this (the game's main) thread; the async variant leaves them queued.
        module->GetResourceManager()->GetEvents().EmitReservedSync(scope.Isolate(), scope.Context(), name, makeArgs(scope.Isolate(), data));
    }
} // namespace

namespace ClientNatives {
    void Attach(RDMP::FwClient *client, const Host *host) {
        g_client = client;
        g_host   = host;
    }

    void Register(Framework::Scripting::Engine *engine) {
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return;
        }
        auto *isolate = scope.Isolate();
        auto context  = scope.Context();
        auto global   = context->Global();
        const auto publish = [&](const char *name, v8::FunctionCallback callback) {
            auto fn = v8::Function::New(context, callback).ToLocalChecked();
            global->Set(context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(), fn).Check();
        };
        publish("getLocalPlayer", &GetLocalPlayerFn);
        publish("getLocalActor", &GetLocalActorFn);
        publish("getElementPosition", &GetElementPositionFn);
        publish("setElementPosition", &SetElementPositionFn);
        publish("getElementHealth", &GetElementHealthFn);
        publish("setElementHealth", &SetElementHealthFn);
        publish("getPlayerName", &GetPlayerNameFn);
        publish("getElementType", &GetElementTypeFn);
        publish("isElement", &IsElementFn);
        publish("getElementsByType", &GetElementsByTypeFn);
        publish("fadeCamera", &FadeCameraFn);
        publish("setCameraTarget", &SetCameraTargetFn);
        publish("toggleAllControls", &ToggleAllControlsFn);
        publish("outputChatBox", &OutputChatBoxFn);
        publish("getTickCount", &GetTickCountFn);
        publish("isKeyDown", &IsKeyDownFn);
        publish("invokeNative", &InvokeNativeFn);
        auto icons = v8::Object::New(isolate);
        for (const auto &icon : RDMP::Shared::kBlipIcons) {
            icons->Set(context, v8::String::NewFromUtf8(isolate, icon.name).ToLocalChecked(), v8::Integer::New(isolate, icon.id)).Check();
        }
        global->Set(context, v8::String::NewFromUtf8Literal(isolate, "BlipIcon"), icons).Check();
        RDMP::Log()->info("RDMP client script natives registered");
    }

    void BeforeUpdate() {
        if (!g_client || !g_host) {
            return;
        }
        // Key transitions while the game window has focus.
        static bool state[256] = {};
        const bool focus       = g_host->game_has_focus() != 0;
        static int lastFocus   = -1;
        if (lastFocus != static_cast<int>(focus)) {
            RDMP::Log()->info("client keys: game focus {}", focus ? "gained" : "lost");
            lastFocus = focus;
        }
        for (const auto &k : Keys()) {
            const bool down = focus && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
            if (down == state[k.vk & 0xff]) {
                continue;
            }
            state[k.vk & 0xff] = down;
            struct KeyEvent {
                const char *name;
                bool down;
            } ev {k.name, down};
            Emit("clientKey",
                 [](v8::Isolate *isolate, const void *p) -> std::vector<v8::Local<v8::Value>> {
                     const auto *e = static_cast<const KeyEvent *>(p);
                     return {v8::String::NewFromUtf8(isolate, e->name).ToLocalChecked(), v8::Boolean::New(isolate, e->down)};
                 },
                 &ev);
        }
        static ULONGLONG last = GetTickCount64();
        const ULONGLONG now   = GetTickCount64();
        const double dt       = static_cast<double>(now - last);
        last                  = now;
        Emit("clientRender",
             [](v8::Isolate *isolate, const void *p) -> std::vector<v8::Local<v8::Value>> {
                 return {v8::Number::New(isolate, *static_cast<const double *>(p))};
             },
             &dt);
    }
} // namespace ClientNatives
