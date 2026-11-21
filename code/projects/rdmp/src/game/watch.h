#pragma once
#include <cstdint>

namespace rdmp {

// Hardware write-watchpoint (DR0) on a 4-byte value, applied to all game threads.
// Every write is logged with the writing instruction, the new value and a call stack.
// Arms asynchronously (a helper thread suspends/updates/resumes the other threads).
void ArmWriteWatch(uintptr_t address, const char* label);

// Same watchpoint, but programmed synchronously on the calling thread (no helper thread): call it from a
// trampoline to catch a write that happens immediately after the hook.
void ArmWriteWatchInline(uintptr_t address, const char* label, uintptr_t chainOffset = 0);

// Hardware execute breakpoint (DR0) on a function: logs the first calls (rcx/rdx/r8, return address), then every
// 1024th. Replaces any armed write watch.
void ArmExecWatch(uintptr_t address, const char* label);
int WatchHitCount();
// Up to 4 execute breakpoints (DR0-DR3) at once; each hit is logged with its index (first 8 per breakpoint).
void ArmExecBreakpoints(const uintptr_t* addrs, int n, const char* label);
// Up to 4 distinct rcx values (first argument) seen by the execute breakpoint (0 = none).
uintptr_t WatchExecRcx(int i);

// Arms the watch on the local player's actor handle (playerInfo + 0x5ec), if player info exists.
void ArmPlayerHandleWatch();

}  // namespace rdmp
