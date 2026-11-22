#pragma once

namespace rdmp {

// Title mode: our own menu files (SCXML, the game's UI format) are loaded with the game's own, from
// <rdmp.ini folder>\ui\native\ (see uifiles.cpp). Install at DLL load, before the game reads its UI.
void InstallUiFiles(const wchar_t* iniPath);
// Game main thread, every frame: keeps our menus' label texts set; frontEnd = before the world load (title):
// also pumps the server browser.
void UiFilesFrame(bool frontEnd);
// UI events ("rdmp.*" from our menus: browser tabs, join, favorite, refresh).
void UiFilesOnEvent(const char* name);

}  // namespace rdmp
