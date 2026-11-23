#pragma once
// The game's key bindings (gabindings.xml): read, rebind through the game's own commit, which applies them to its input
// system (not saved to the game's file: the menu keeps them in rdmp.ini). See bindings.cpp. Main thread.
#include <cstdint>

#include "shared/fw_api.h"

namespace rdmp {

int ReadGameBindings(fw::GameBinding* out, int max);
bool SetGameBinding(const char* action, int slot, int32_t type, int32_t input);

}  // namespace rdmp
