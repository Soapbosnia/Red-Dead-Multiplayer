#pragma once
#include <cstdint>

namespace rdmp {

// Hooks the script VM's native call site (0x1401E4353 "call rdx") through a trampoline.
// With [debug] log_natives=1, logs every native handler the first time scripts call it (in call order).
void InstallNativeHook(bool logNatives);

// Total native calls the game's scripts made (sanity checks).
uint64_t NativeCallCount();

// Logs handler, args and result of the next `count` native calls (debug: what a script does after an event).
void TraceNextNativeCalls(int count);

}  // namespace rdmp
