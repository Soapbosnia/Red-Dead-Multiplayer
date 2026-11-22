// Our own menus in the game's UI language (title mode). The game's menus are SCXML files ("UserInterface.sc" and the
// files it <include>s, in content.rpf): screens, lists, labels, options, prompt strips and their event transitions.
// The game opens every one of them through FUN_1400ec440(name, ...). We detour it:
//   * "UserInterface.sc": read the game's file, add <include src="rdmp/Main.sc"> before tunings.sc, serve that.
//   * "rdmp/...": served from our folder <rdmp.ini folder>\ui\native\.
// Served buffers go through rage's memory device ("memory:$<ptr>,<size>,0:<name>"; FUN_1400ec440 treats a name with
// ':' as an absolute path and caches what it opened). Nothing is written to the game's files.
#include "uifiles.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

#include "events.h"
#include "log.h"
#include "patch.h"

namespace rdmp {
namespace {

using OpenFn = void* (*)(const char*, void*, void*, void*);
OpenFn g_open = nullptr;
wchar_t g_dir[MAX_PATH] = L"";        // ...\ui\native\ (with trailing backslash)
std::deque<std::string> g_buffers;    // served file contents (stable addresses for the memory device)
bool g_logNames = false;

std::string ReadOurFile(const char* rel) {
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, L"%s%S", g_dir, rel);
    for (wchar_t* p = path; *p; ++p)
        if (*p == L'/') *p = L'\\';
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Reads a whole game stream (fiStream: +0 device, +8 handle; device vt+0x70 = size; FUN_1400c7c20 = read;
// FUN_1400c7e90 = close).
std::string ReadStream(void* stream) {
    if (!stream) return {};
    auto* s = static_cast<uintptr_t*>(stream);
    const uintptr_t dev = s[0];
    const int size = reinterpret_cast<int (*)(uintptr_t, uintptr_t)>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(dev) + 0x70))(dev, s[1]);
    std::string out;
    if (size > 0) {
        out.resize(size);
        reinterpret_cast<int (*)(void*, void*, int)>(GameAddr(0x1400C7C20))(stream, out.data(), size);
    }
    reinterpret_cast<void (*)(void*)>(GameAddr(0x1400C7E90))(stream);
    return out;
}

// Replaces the first `from` that follows `anchor`.
bool ReplaceAfter(std::string& s, const char* anchor, const char* from, const char* to) {
    const size_t a = s.find(anchor);
    const size_t at = a == std::string::npos ? a : s.find(from, a);
    if (at == std::string::npos) return false;
    s.replace(at, std::strlen(from), to);
    return true;
}

void* Serve(const std::string& content, const char* name, void* a2, void* a3, void* a4) {
    g_buffers.push_back(content);
    const std::string& b = g_buffers.back();
    char path[512];
    sprintf_s(path, "memory:$%p,%d,%d:%s", static_cast<const void*>(b.data()), static_cast<int>(b.size()), 0, name);
    return g_open(path, a2, a3, a4);
}

void* OpenHook(const char* name, void* a2, void* a3, void* a4) {
    if (!name) return g_open(name, a2, a3, a4);
    if (g_logNames) Log("ui files: open \"%s\"", name);
    if (_strnicmp(name, "rdmp/", 5) == 0) {
        const std::string s = ReadOurFile(name + 5);
        if (s.empty()) {
            Log("ui files: %s not found in ui\\native", name);
            return nullptr;
        }
        Log("ui files: serving %s (%zu bytes)", name, s.size());
        return Serve(s, name, a2, a3, a4);
    }
    if (_stricmp(name, "UserInterface.sc") == 0) {
        std::string s = ReadStream(g_open(name, a2, a3, a4));
        const char* anchor = "<include src=\"tunings.sc\">";
        const size_t at = s.find(anchor);
        if (at == std::string::npos) {
            Log("ui files: UserInterface.sc has no tunings include - not patched");
            return s.empty() ? nullptr : Serve(s, name, a2, a3, a4);
        }
        s.insert(at, "<include src=\"rdmp/Main.sc\"></include>\n    ");
        Log("ui files: UserInterface.sc patched (rdmp/Main.sc included)");
        return Serve(s, name, a2, a3, a4);
    }
    if (_stricmp(name, "Boot.sc") == 0) {
        // The title's mode list (SinglePlayerList in StartScreen2: Normal / Hardcore / Undead Nightmare) becomes our
        // main menu (ui/native/MainMenuList.sc).
        std::string s = ReadStream(g_open(name, a2, a3, a4));
        const size_t from = s.find("<UIScrollableList id=\"SinglePlayerList\"");
        const size_t end = from == std::string::npos ? from : s.find("</UIScrollableList>", from);
        const std::string ours = ReadOurFile("MainMenuList.sc");
        if (end == std::string::npos || ours.empty()) {
            Log("ui files: Boot.sc mode list %s - not patched", ours.empty() ? "replacement missing" : "not found");
        } else {
            s.replace(from, end + std::strlen("</UIScrollableList>") - from, ours);
            Log("ui files: Boot.sc patched (main menu)");
        }
        // One main menu: the boot goes straight to it (StartScreen2) instead of the "press Play" title, and its Back
        // (Esc) asks to quit.
        if (ReplaceAfter(s, "<transition event=\"benchmarkComplete\"", "target=\"StartScreen1\"", "target=\"StartScreen2\""))
            Log("ui files: Boot.sc patched (boot to the main menu)");
        const char* stripTag = "<UIPromptStrip id=\"StartScreen2PromptStrip\"";
        const size_t strip = s.find(stripTag);
        const size_t t0 = strip == std::string::npos ? strip : s.find("<transition event=\"@UI.CANCEL*RELEASED\"", strip);
        const size_t t1 = t0 == std::string::npos ? t0 : s.find("</transition>", t0);
        if (t1 != std::string::npos) {
            s.replace(t0, t1 + std::strlen("</transition>") - t0,
                      "<transition event=\"@UI.CANCEL*RELEASED\" consume=\"true\"><action expr=\"Enter(QuitGame)\"></action></transition>");
            ReplaceAfter(s, stripTag, "text=\"Common_Back\"", "text=\"Common_Quit\"");
            ReplaceAfter(s, stripTag, "text=\"Common_Play\"", "text=\"Common_Select\"");
            Log("ui files: Boot.sc patched (main menu Esc = quit)");
        }
        return s.empty() ? nullptr : Serve(s, name, a2, a3, a4);
    }
    return g_open(name, a2, a3, a4);
}

// FUN_1400ec440 starts with push r13; push r15; sub rsp,0x258 (11 bytes) and then mov r15,[rip -> 0x142ABC0C0]
// (7 bytes): the trampoline re-encodes that load with an absolute address.
bool InstallOpenHook() {
    const uintptr_t at = GameAddr(0x1400EC440);
    static const uint8_t kExpect[18] = {0x41, 0x55, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x58, 0x02, 0x00, 0x00,
                                        0x4C, 0x8B, 0x3D, 0x6E, 0xFC, 0x9C, 0x02};
    if (std::memcmp(reinterpret_cast<void*>(at), kExpect, sizeof(kExpect)) != 0) return false;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    size_t n = 0;
    std::memcpy(t, kExpect, 11);
    n = 11;
    const uint64_t cache = GameAddr(0x142ABC0C0);
    t[n++] = 0x49; t[n++] = 0xBF;  // mov r15, imm64
    std::memcpy(t + n, &cache, 8); n += 8;
    t[n++] = 0x4D; t[n++] = 0x8B; t[n++] = 0x3F;  // mov r15, [r15]
    t[n++] = 0xFF; t[n++] = 0x25; t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = 0;  // jmp [rip+0]
    const uint64_t back = at + 18;
    std::memcpy(t + n, &back, 8);
    g_open = reinterpret_cast<OpenFn>(t);
    uint8_t code[18];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0x48; code[1] = 0xB8;
    const uint64_t h = reinterpret_cast<uint64_t>(&OpenHook);
    std::memcpy(code + 2, &h, 8);
    code[10] = 0xFF; code[11] = 0xE0;
    return WriteCode(at, code, sizeof(code));
}

// Label texts of our menus (UILabel = text widget vt 0x141C61950: literal text = +0x80 "key dirty" 0, +0x88 char*,
// then vt+0x3d0 to re-layout). Widgets by id: FUN_1400ef100(*0x142AAC8C0, id).
struct Text { const char* id; const char* text; };
const Text kTexts[] = {
    {"RdmpMenu_Browser", "Server Browser"},
    {"RdmpMenu_Direct", "Direct Connect"},
    {"RdmpMenu_MpSettings", "Multiplayer Settings"},
    {"logo", "RED DEAD MULTIPLAYER"},  // the title logo is a text field in the game's "Redemption" font
};

// Flash variables of a UI movie (what SetFlashInt/Float/Bool/String do, FUN_1400ed8a0..FUN_1400eda00): movie wrapper =
// ui list *0x142C45610 entry FUN_14011a000(list, movie); slot = FUN_140116610(wrapper, path): +0 value, +8 string
// hash, +0x10 type (1 string, 2 bool, 4 float); ints go through FUN_1400f6b30(movie, path, value).
uintptr_t FlashSlot(const char* movie, const char* path) {
    const uintptr_t list = *reinterpret_cast<uintptr_t*>(GameAddr(0x142C45610));
    if (!list) return 0;
    const int idx = reinterpret_cast<char (*)(uintptr_t, const char*)>(GameAddr(0x14011A000))(list, movie);
    if (idx < 0 || idx >= *reinterpret_cast<int*>(list + 0x180)) return 0;
    const uintptr_t wrapper = *reinterpret_cast<uintptr_t*>(list + 0x30 + idx * 8ULL);
    return wrapper ? reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x140116610))(wrapper, path) : 0;
}
std::deque<std::string> g_flashStrings;
void FlashSetString(const char* movie, const char* path, const std::string& text) {
    const uintptr_t slot = FlashSlot(movie, path);
    if (!slot) return;
    g_flashStrings.push_back(text);
    const char* v = g_flashStrings.back().c_str();
    *reinterpret_cast<const char**>(slot) = v;
    *reinterpret_cast<uint32_t*>(slot + 0x10) = 1;
    const uint32_t h = reinterpret_cast<uint32_t (*)(const char*, int)>(GameAddr(0x1400C7510))(v, 0);
    *reinterpret_cast<uint32_t*>(slot + 8) = ((h * 9) >> 11 ^ h * 9) * 0x8001;
}
void FlashSetInt(const char* movie, const char* path, int v) {
    reinterpret_cast<void (*)(const char*, const char*, int)>(GameAddr(0x1400F6B30))(movie, path, v);
}
void FlashSetFloat(const char* movie, const char* path, float v) {
    if (const uintptr_t slot = FlashSlot(movie, path)) {
        *reinterpret_cast<float*>(slot) = v;
        *reinterpret_cast<uint32_t*>(slot + 0x10) = 4;
    }
}
void FlashSetBool(const char* movie, const char* path, bool v) {
    if (const uintptr_t slot = FlashSlot(movie, path)) {
        *reinterpret_cast<uint8_t*>(slot) = v ? 1 : 0;
        *reinterpret_cast<uint32_t*>(slot + 0x10) = 2;
    }
}

// Debug ([debug] ui_exec=1): the lines of <rdmp.ini folder>\ui_exec.txt run each time the file changes:
//   i|f|b <movie> <path> <value>     s <movie> <path> <text...>     e <ui event>
bool g_exec = false;
wchar_t g_execPath[MAX_PATH] = L"";
void ExecFile() {
    static ULONGLONG lastCheck = 0;
    static FILETIME lastWrite = {};
    if (GetTickCount64() - lastCheck < 300) return;
    lastCheck = GetTickCount64();
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_execPath, GetFileExInfoStandard, &a)) return;
    if (CompareFileTime(&a.ftLastWriteTime, &lastWrite) == 0) return;
    lastWrite = a.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_execPath, L"r") != 0 || !f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char kind = 0, movie[64] = {}, var[192] = {};
        int used = 0;
        if (sscanf_s(line, " %c %63s %191s %n", &kind, 1, movie, 64, var, 192, &used) < 2) continue;
        const char* rest = used ? line + used : "";
        switch (kind) {
            case 'i': FlashSetInt(movie, var, atoi(rest)); break;
            case 'f': FlashSetFloat(movie, var, static_cast<float>(atof(rest))); break;
            case 'b': FlashSetBool(movie, var, atoi(rest) != 0); break;
            case 's': FlashSetString(movie, var, rest); break;
            case 'e': PostUiEvent(movie); break;
            default: continue;
        }
        Log("ui exec: %s", line);
    }
    fclose(f);
}

}  // namespace

void UiFilesFrame() {
    if (!g_open) return;
    if (g_exec) ExecFile();
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
    if (!root) return;
    auto find = reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100));
    const uintptr_t textVt = GameAddr(0x141C61950);
    for (const Text& t : kTexts) {
        const uintptr_t w = find(root, t.id);
        if (!w || *reinterpret_cast<uintptr_t*>(w) != textVt) continue;
        if (*reinterpret_cast<uint8_t*>(w + 0x80) == 0 && *reinterpret_cast<const char**>(w + 0x88) == t.text) continue;
        *reinterpret_cast<uint8_t*>(w + 0x80) = 0;
        *reinterpret_cast<const char**>(w + 0x88) = t.text;
        reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(w) + 0x3d0))(w);
    }
}

void InstallUiFiles(const wchar_t* iniPath) {
    if (!GetPrivateProfileIntW(L"game", L"title_screen", 0, iniPath)) return;
    g_logNames = GetPrivateProfileIntW(L"debug", L"ui_file_log", 0, iniPath) != 0;
    g_exec = GetPrivateProfileIntW(L"debug", L"ui_exec", 0, iniPath) != 0;
    wcscpy_s(g_execPath, iniPath);
    if (wchar_t* slash = wcsrchr(g_execPath, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_execPath), L"ui_exec.txt");
    wcscpy_s(g_dir, iniPath);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_dir), L"ui\\native\\");
    Log("ui files: %s (our menus from %S)", InstallOpenHook() ? "hook installed" : "hook FAILED (bytes mismatch)", g_dir);
}

}  // namespace rdmp
