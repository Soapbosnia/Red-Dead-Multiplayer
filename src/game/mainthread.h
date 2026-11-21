#pragma once
#include <functional>

namespace rdmp {

// Detours the script VM runner (FUN_1402066d0, runs every frame on the game's main thread, even with no scripts)
// so queued tasks run there. Install before the game starts running.
void InstallMainThreadHook();

// Queues `fn` to run on the game's main thread at the next VM-runner call (safe place to call engine functions).
// Note: the VM runner is called several times per frame.
void RunOnMainThread(std::function<void()> fn);

// Registers `fn` to run on the main thread once per rendered frame (frame boundary = the game clock at 0x142314d3c
// changed). `fn` gets the game clock in seconds.
void AddFrameCallback(std::function<void(float gameTime)> fn);

}  // namespace rdmp
