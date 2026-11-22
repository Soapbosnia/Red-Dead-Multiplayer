#pragma once

namespace rdmp {

// Replaces the game's main loop (Game::Run), its loading-mode frame and the splash part of Game::Init with ours
// (rdmp.ini [game] own_loop=1). Call at DLL load, before the game initialises.
void InstallGameFlow(const wchar_t* iniPath);
bool OwnGameLoopActive();
bool OwnBootGateOpen();
// [game] title_screen=1: the game boots to its own title screen and loads the world only once this is called (joining a
// server). Thread-safe.
bool GameflowTitleMode();
bool GameflowWorldLoadStarted();
void GameflowRequestWorldLoad();
// UI event notification (from the sandbox event hook): later loads (story/missions) are finished by us too.
void GameflowOnUiEvent(const char* name);

}  // namespace rdmp
