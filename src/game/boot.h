#pragma once
#include <windows.h>

namespace rdmp {

// Reads [boot] settings from rdmp.ini (next to the DLL) and installs the boot patches.
void InstallBootPatches(HMODULE self);

}  // namespace rdmp
