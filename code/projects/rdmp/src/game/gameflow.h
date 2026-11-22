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
// The pause menu's Disconnect (title mode): the loaded world goes away and the game is back at its title, as after the
// boot; the next GameflowRequestWorldLoad brings a world in again. See gameflow.cpp GameflowUnloadWorld.
void GameflowUnloadWorld();
// [game] disconnect_unload=1: Disconnect unloads the world (else it stays loaded behind the main menu).
bool GameflowUnloadOnDisconnect();
// The world was unloaded and its reload is held until the next join (no finishing of the level load meanwhile).
bool GameflowWorldParked();
// The unload's session restart has run (the title front end can be shown).
bool GameflowUnloadDone();
// UI event notification (from the sandbox event hook): later loads (story/missions) are finished by us too.
void GameflowOnUiEvent(const char* name);

}  // namespace rdmp
