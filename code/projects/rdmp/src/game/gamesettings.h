#pragma once
// The game's own settings (shared/game_options.h), read and applied through the game's own code: what its options
// menu does, without the menu. Main thread.
#include <cstdint>

namespace rdmp {

// Current values: loaded = rage::LoadedOptions (0x70 bytes), graphics = rage::GraphicsOptions (0x5c bytes).
bool ReadGameOptions(uint8_t* loaded, uint8_t* graphics);
// Applies them to the running game and has the game save its own options.xml / graphicsOptions.xml. Either may be
// null (left as it is).
bool ApplyGameOptions(const uint8_t* loaded, const uint8_t* graphics);

}  // namespace rdmp
