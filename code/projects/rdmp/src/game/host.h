#pragma once
#include "shared/fw_api.h"

namespace rdmp {

// The Host table rdmp.dll gives rdmp_client.dll: what client script natives can do in the game (main thread only).
const fw::Host* GameHost();

// Once per frame on the main thread, before the client update: finishes deferred on-screen lines.
void HostFrame();

}  // namespace rdmp
