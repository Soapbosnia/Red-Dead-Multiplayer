/*
 * Client script API: registration and the per-frame events.
 *
 * Events (CLIENT):
 *   clientRender(msSinceLastFrame)         every frame; dx* drawing goes here
 *   clientKey(key, down)                   while the game has focus and the chat input is closed
 *   clientClick(button, state, x, y)       while the cursor is shown (showCursor): "left"|"right"|"middle", "down"|"up"
 * The Framework's own events (chatMessage, ...) are available too.
 */

#include <utils/safe_win32.h>

#include "client/api.h"
#include "client/client_natives.h"
#include "client/fw_client.h"
#include "client/hud.h"
#include "client/overlay.h"

#include <scripting/builtins/entity.h>
#include <scripting/engine_scope.h>
#include <scripting/resource/resource_manager.h>
#include <v8pp/convert.hpp>

#include <string_view>

namespace RDMP::ClientApi {
    namespace {
        FwClient *g_client           = nullptr;
        const rdmp::fw::Host *g_host = nullptr;
    } // namespace

    FwClient *Client() {
        return g_client;
    }
    const rdmp::fw::Host *Host() {
        return g_host;
    }

    uint64_t LocalId() {
        auto *me = g_client ? g_client->Owned() : nullptr;
        return me ? static_cast<uint64_t>(me->GetNetworkID()) : 0;
    }

    uint64_t ElementId(const Args &args, int i) {
        if (!ScriptApi::Has(args, i) || !args[i]->IsObject()) {
            return 0;
        }
        auto *handle = v8pp::from_v8<Framework::Scripting::Builtins::Entity *>(args.GetIsolate(), args[i]);
        return handle ? handle->GetId() : 0;
    }

    v8::Local<v8::Value> Wrap(v8::Isolate *isolate, uint64_t id) {
        if (!id || !g_client) {
            return v8::Null(isolate);
        }
        return g_client->WrapScriptEntity(isolate, id);
    }

    bool LocalPlayer(float pos[3], float *heading, float *health, uint32_t *actor) {
        return g_host && g_host->get_local_player && g_host->get_local_player(pos, heading, health, actor);
    }
} // namespace RDMP::ClientApi

namespace {
    using namespace RDMP;

    // Emits a reserved event to every client resource, synchronously (handlers run now, on the main thread).
    template <typename MakeArgs>
    void Emit(const char *name, MakeArgs &&makeArgs) {
        auto *client = ClientApi::Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine || !module->GetResourceManager()) {
            return;
        }
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return;
        }
        module->GetResourceManager()->GetEvents().EmitReservedSync(scope.Isolate(), scope.Context(), name, makeArgs(scope.Isolate()));
    }
} // namespace

namespace RDMP::ClientApi {
    bool EmitChatSend(const std::string &text) {
        auto *client = Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine || !module->GetResourceManager()) {
            return true;
        }
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return true;
        }
        std::vector<v8::Local<v8::Value>> args {RDMP::ScriptApi::JsString(scope.Isolate(), text)};
        return module->GetResourceManager()->GetEvents().EmitReservedSync(scope.Isolate(), scope.Context(), "chatSend", args);
    }
} // namespace RDMP::ClientApi

namespace RDMP::ClientApi {
    bool EmitClientPlayerDamage(uint64_t attacker, int weapon, int bodyPart, float loss) {
        auto *client = Client();
        auto *module = client ? client->GetScriptingModule() : nullptr;
        auto *engine = module ? module->GetScriptingEngine() : nullptr;
        if (!engine || !module->GetResourceManager()) {
            return true;
        }
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return true;
        }
        auto *isolate = scope.Isolate();
        std::vector<v8::Local<v8::Value>> args {attacker ? Wrap(isolate, attacker) : v8::Null(isolate).As<v8::Value>(),
                                                v8::Integer::New(isolate, weapon), v8::Integer::New(isolate, bodyPart),
                                                v8::Number::New(isolate, loss)};
        return module->GetResourceManager()->GetEvents().EmitReservedSync(isolate, scope.Context(), "clientPlayerDamage", args);
    }
} // namespace RDMP::ClientApi

namespace ClientNatives {
    void Attach(RDMP::FwClient *client, const rdmp::fw::Host *host) {
        RDMP::ClientApi::g_client = client;
        RDMP::ClientApi::g_host   = host;
    }

    void Register(Framework::Scripting::Engine *engine) {
        Framework::Scripting::EngineScope scope(engine);
        if (!scope.Valid()) {
            return;
        }
        const auto publish = RDMP::ScriptApi::GlobalPublisher(scope.Isolate(), scope.Context());
        RDMP::ScriptApi::RegisterShared(scope.Isolate(), scope.Context(), publish);
        RDMP::ClientApi::RegisterPlayerApi(publish);
        RDMP::ClientApi::RegisterHudApi(publish);
        RDMP::ClientApi::RegisterInputApi(publish);
        RDMP::ClientApi::RegisterNativeApi(publish);
        RDMP::Log()->info("RDMP client script API registered");
    }

    void BeforeUpdate() {
        const auto *host = RDMP::ClientApi::Host();
        if (!RDMP::ClientApi::Client() || !host) {
            return;
        }
        // Keys, only while the player is driving the game (not typing in the chat box).
        static bool state[256] = {};
        const bool live        = host->game_has_focus() != 0 && !RDMP::Overlay::UiHasKeyboard();
        for (const auto &k : RDMP::ClientApi::Keys()) {
            const bool down = live && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
            if (down == state[k.vk & 0xff]) {
                continue;
            }
            state[k.vk & 0xff] = down;
            RDMP::ClientApi::DispatchBinds(k.name, down);
            Emit("clientKey", [&](v8::Isolate *isolate) {
                return std::vector<v8::Local<v8::Value>> {RDMP::ScriptApi::JsString(isolate, k.name), v8::Boolean::New(isolate, down)};
            });
        }
        // Clicks while the cursor is shown.
        RDMP::Overlay::Click click {};
        while (RDMP::Overlay::PollClick(click)) {
            static const char *kButtons[] = {"left", "right", "middle"};
            Emit("clientClick", [&](v8::Isolate *isolate) {
                return std::vector<v8::Local<v8::Value>> {RDMP::ScriptApi::JsString(isolate, kButtons[click.button % 3]),
                                                          RDMP::ScriptApi::JsString(isolate, click.down ? "down" : "up"),
                                                          v8::Number::New(isolate, click.x), v8::Number::New(isolate, click.y)};
            });
        }
        static ULONGLONG last = GetTickCount64();
        const ULONGLONG now   = GetTickCount64();
        const double dt       = static_cast<double>(now - last);
        last                  = now;
        Emit("clientRender", [&](v8::Isolate *isolate) {
            return std::vector<v8::Local<v8::Value>> {v8::Number::New(isolate, dt)};
        });
    }
} // namespace ClientNatives
