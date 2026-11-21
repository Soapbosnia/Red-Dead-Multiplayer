#pragma once

namespace rdmp {

// rdmp.ini [debug] watch_player_handle: arm the player-handle write watch on "loadStart".
extern bool g_watchOnLoad;

// Starts a watcher thread that hooks the UI manager's post-event method once the UI exists,
// and logs every UI event (e.g. "fileStartNewGame") to rdmp.log.
void StartEventLogger();

// Posts a named UI event through the game's own post-event method (same call the game uses).
// Returns false if the UI manager doesn't exist yet.
bool PostUiEvent(const char* name);

}  // namespace rdmp
