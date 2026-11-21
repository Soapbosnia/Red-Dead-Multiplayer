#pragma once
// RDMP client script natives (MTA-style client API) and the per-frame client script events.
#include "shared/fw_api.h"

namespace Framework::Scripting {
    class Engine;
}

namespace RDMP {
    class FwClient;
}

namespace ClientNatives {
    // The client and the game's Host table (nullptr, nullptr on shutdown).
    void Attach(RDMP::FwClient *client, const rdmp::fw::Host *host);
    // Publishes the natives into a script context (FwClient::ModuleRegister).
    void Register(Framework::Scripting::Engine *engine);
    // Once per frame before the client update: emits "clientKey" (key, down) and "clientRender" (ms since last).
    void BeforeUpdate();
} // namespace ClientNatives
