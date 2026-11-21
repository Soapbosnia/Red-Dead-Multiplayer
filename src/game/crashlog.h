#pragma once

namespace rdmp {

// Logs access violations / illegal instructions that happen in game code (address, registers, call stack)
// to rdmp.log before the game's own crash handler takes over.
void InstallCrashLogger();

}  // namespace rdmp
