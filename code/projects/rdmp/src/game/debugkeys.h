#pragma once

namespace rdmp {

// Debug hotkeys (only while the game window is focused):
//   F5 = ask the game to finish the loading screen (sets the loader's "finish loading" request bit)
//   F7 = log local player state (player info, actor handle, actor pointer)
void StartDebugKeys();

}  // namespace rdmp
