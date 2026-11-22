#pragma once

namespace rdmp {

// Debug ([debug] ui_widget_dump=1): logs every named UI widget (name, class vtable, hidden flag, parent, text) to
// ui_widgets.txt on a few UI events. Used to map the game's own menus.
void InstallUiDump(const wchar_t* iniPath);
void UiDumpOnEvent(const char* name);

}  // namespace rdmp
