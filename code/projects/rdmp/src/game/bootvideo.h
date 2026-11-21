#pragma once

namespace rdmp {

// Hooks BinkOpen (logs every video the game opens; rdmp.ini [sandbox] skip_videos=1 refuses them).
void InstallBootVideoHook(const wchar_t* iniPath);

}  // namespace rdmp
