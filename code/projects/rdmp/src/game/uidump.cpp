// Debug ([debug] ui_widget_dump=1): writes every named UI widget of the loaded menus to ui_widgets.txt (next to
// rdmp.ini) when one of a few UI events arrives, to map the game's own menus (re/docs/FRONTEND_MAP.md).
//
// RE (FUN_1400ef100 = find widget by name): root = *(uiMgr *0x142AAC8B8 + 0xa0); root+8 = bucket array, root+0x10 =
// u16 bucket count; node = {+0 name, +8 u16 name length (0 = empty name), +0x10 widget, +0x18 next}.
// Widget: +0x28 u16 flags (0x800 = hidden, FUN_1400fe810), vtable +0x218 = parent. Text widgets (vtable 0x141C61950)
// keep their localisation key hash at +0x88; FUN_1400fb020(*0x142C45608, hash) resolves it.
#include "uidump.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "log.h"
#include "memcheck.h"
#include "events.h"
#include "patch.h"

namespace rdmp {
namespace {

bool g_enabled = false;
wchar_t g_outPath[MAX_PATH] = L"";
char g_openOnTitle[64] = "";  // [debug] ui_open: screen pushed when the title screen is up (FUN_1400f2d50)

uintptr_t Rd(uintptr_t a) { return IsReadable(a, 8) ? *reinterpret_cast<uintptr_t*>(a) : 0; }

void Dump(const char* reason) {
    const uintptr_t ui = Rd(GameAddr(0x142AAC8B8));
    const uintptr_t root = ui ? Rd(ui + 0xa0) : 0;
    if (!root) return;
    const uintptr_t buckets = Rd(root + 8);
    const unsigned count = *reinterpret_cast<uint16_t*>(root + 0x10);
    if (!buckets || !count) return;
    std::map<std::string, uintptr_t> widgets;
    std::map<uintptr_t, std::string> names;
    for (unsigned b = 0; b < count; ++b) {
        for (uintptr_t n = Rd(buckets + b * 8ULL); n && IsReadable(n, 0x20); n = Rd(n + 0x18)) {
            const uintptr_t w = Rd(n + 0x10);
            const uintptr_t s = *reinterpret_cast<uint16_t*>(n + 8) ? Rd(n) : 0;
            if (!w || !s || !IsReadable(s, 1)) continue;
            std::string name(reinterpret_cast<const char*>(s), strnlen(reinterpret_cast<const char*>(s), 80));
            widgets[name] = w;
            names[w] = name;
        }
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_outPath, L"a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n===== %02d:%02d:%02d \"%s\": %zu widgets =====\n", t.wHour, t.wMinute, t.wSecond, reason, widgets.size());
    const uintptr_t textVt = GameAddr(0x141C61950);
    const uintptr_t strings = Rd(GameAddr(0x142C45608));
    auto localized = reinterpret_cast<const char* (*)(uintptr_t, uint32_t)>(GameAddr(0x1400FB020));
    for (const auto& [name, w] : widgets) {
        const uintptr_t vt = Rd(w);
        const uint16_t flags = IsReadable(w + 0x28, 2) ? *reinterpret_cast<uint16_t*>(w + 0x28) : 0;
        uintptr_t parent = 0;
        if (vt && IsReadable(vt + 0x218, 8)) parent = reinterpret_cast<uintptr_t (*)(uintptr_t)>(Rd(vt + 0x218))(w);
        const auto pit = names.find(parent);
        char text[96] = "";
        if (vt == textVt && strings) {
            const uint32_t key = *reinterpret_cast<uint32_t*>(w + 0x88);
            const char* s = localized(strings, key);
            if (s && IsReadable(reinterpret_cast<uintptr_t>(s), 1)) sprintf_s(text, " text=\"%.80s\"", s);
        }
        fprintf(f, "%-44s vt=%llx flags=%04x%s parent=%s%s\n", name.c_str(),
                static_cast<unsigned long long>(vt ? vt - GameBase() + 0x140000000ULL : 0), flags,
                (flags & 0x800) ? " HIDDEN" : "", pit != names.end() ? pit->second.c_str() : (parent ? "?" : "-"), text);
    }
    fclose(f);
    Log("ui dump: %zu widgets at \"%s\"", widgets.size(), reason);
}

}  // namespace

void InstallUiDump(const wchar_t* iniPath) {
    wchar_t w[64] = {};
    GetPrivateProfileStringW(L"debug", L"ui_open", L"", w, 64, iniPath);
    for (int i = 0; i < 63 && w[i]; ++i) g_openOnTitle[i] = static_cast<char>(w[i]);
    g_enabled = GetPrivateProfileIntW(L"debug", L"ui_widget_dump", 0, iniPath) != 0;
    if (!g_enabled) return;
    wcscpy_s(g_outPath, iniPath);
    if (wchar_t* slash = wcsrchr(g_outPath, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_outPath), L"ui_widgets.txt");
    DeleteFileW(g_outPath);
    Log("ui dump: on");
}

void UiDumpOnEvent(const char* name) {
    static bool opened = false;
    if (g_openOnTitle[0] && !opened && std::strcmp(name, "OutTransitionComplete") == 0) {
        opened = true;
        const uintptr_t ui = Rd(GameAddr(0x142AAC8B8));
        const uintptr_t root = ui ? Rd(ui + 0xa0) : 0;
        const uintptr_t w = root ? reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100))(root, g_openOnTitle) : 0;
        Log("ui open: \"%s\" = %p", g_openOnTitle, reinterpret_cast<void*>(w));
        if (!w) {  // not a widget: post it as a UI event instead
            PostUiEvent(g_openOnTitle);
            Log("ui open: posted event \"%s\"", g_openOnTitle);
        }
        if (w) reinterpret_cast<void (*)(uintptr_t, uintptr_t, int)>(GameAddr(0x1400F2D50))(Rd(ui + 0x90), w, 1);
    }
    if (!g_enabled) return;
    static const char* kTriggers[] = {"OutTransitionComplete", "PauseBrush3Begin", "startScreenExit", "InTransitionComplete"};
    for (const char* t : kTriggers)
        if (std::strcmp(name, t) == 0) Dump(name);
}

}  // namespace rdmp
