#pragma once
/*
 * RDMP client script API internals. Client scripts run on the game's main thread (rdmp.dll pumps the Framework
 * client from its frame callback), so these functions call straight into the game through the Host table.
 *
 *   api_core.cpp     Attach / Register / per-frame events (client_natives.h)
 *   api_players.cpp  local player, elements, camera, controls, killPed
 *   api_hud.cpp      dx* drawing, screen / cursor, chat box, engine HUD text, nametags
 *   api_input.cpp    keys
 *   api_natives.cpp  invokeNative (any game native)
 */

#include <cstdint>
#include <string>
#include <vector>

#include "shared/fw_api.h"
#include "shared/script_api.h"

namespace RDMP {
    class FwClient;
}

namespace RDMP::ClientApi {
    using ScriptApi::Args;

    FwClient *Client();
    const rdmp::fw::Host *Host();

    uint64_t LocalId();                                       // our own player element, 0 until assigned
    uint64_t ElementId(const Args &args, int i);              // network id of an element handle, 0 if not one
    v8::Local<v8::Value> Wrap(v8::Isolate *isolate, uint64_t id);
    bool LocalPlayer(float pos[3], float *heading = nullptr, float *health = nullptr, uint32_t *actor = nullptr);

    // Keys by script name ("f8", "e", "space", "mouse1", ...).
    struct KeyName {
        int vk;
        const char *name;
    };
    const std::vector<KeyName> &Keys();
    // bindKey handlers for a key that went down / up (api_input.cpp); main thread, outside any engine scope.
    void DispatchBinds(const char *key, bool down);

    void RegisterPlayerApi(const ScriptApi::Publisher &publish);
    void RegisterHudApi(const ScriptApi::Publisher &publish);
    void RegisterInputApi(const ScriptApi::Publisher &publish);
    void RegisterNativeApi(const ScriptApi::Publisher &publish);
} // namespace RDMP::ClientApi
