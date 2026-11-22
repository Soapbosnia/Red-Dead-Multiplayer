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
#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "events.h"
#include "gameflow.h"
#include "log.h"
#include "netclient.h"
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

// The first `what` at or after `from` that is not inside an XML comment (the game's files comment out whole blocks).
size_t FindOutsideComments(const std::string& s, const char* what, size_t from) {
    size_t pos = from;
    for (;;) {
        const size_t hit = s.find(what, pos);
        const size_t open = s.find("<!--", pos);
        if (hit == std::string::npos || open == std::string::npos || hit < open) return hit;
        const size_t close = s.find("-->", open + 4);
        if (close == std::string::npos) return std::string::npos;
        pos = close + 3;
    }
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
    if (g_logNames) {  // debug: what we served, as <rdmp.ini folder>\ui_served\<name with / -> _>
        std::wstring dir(g_dir);
        dir = dir.substr(0, dir.size() - std::wstring(L"ui\\native\\").size()) + L"ui_served\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring file = dir;
        for (const char* p = name; *p; ++p) file += (*p == '/' || *p == '\\') ? L'_' : static_cast<wchar_t>(*p);
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
    }
    char path[512];
    sprintf_s(path, "memory:$%p,%d,%d:%s", static_cast<const void*>(b.data()), static_cast<int>(b.size()), 0, name);
    return g_open(path, a2, a3, a4);
}

// The multiplayer pause menu (PauseMenu list in PauseMenuScene.sc): the single-player items move out of the list
// into a container that is never entered (the game's C++ still finds them by name - FUN_140628860 falls back to the
// root "UserInterface" widget when one is missing, so they must exist), and DISCONNECT (with a confirmation) is added.
void PatchPauseMenu(std::string& s) {
    const size_t list = s.find("<UIList id=\"PauseMenu\"");
    if (list == std::string::npos) {
        Log("ui files: pause menu list not found - not patched");
        return;
    }
    std::string moved;  // (PM_Network, PM_SocialClub and PM_Leaderboards are commented out in the game's file)
    for (const char* id : {"PM_Weapons", "PM_Outfits", "PM_Journal", "PM_Stats", "PM_Load"}) {
        const std::string open = std::string("<UILabel id=\"") + id + "\"";
        const size_t a = s.find(open, list);
        const size_t b = a == std::string::npos ? a : s.find("</UILabel>", a);
        if (b == std::string::npos) continue;
        moved += "      " + s.substr(a, b + std::strlen("</UILabel>") - a) + "\n";
        s.erase(a, b + std::strlen("</UILabel>") - a);
    }
    // The one menu (MTA-style, title and pause): Map (in game), Server Browser, Direct Connect, Settings (the game's
    // Options), Disconnect (in game), Quit. Which ones show is set on rdmp.pause (UiFilesOnEvent) before the list is
    // entered; Back goes through rdmp.back (quit prompt on the title, close in game).
    auto item = [](const char* id, const char* actions) {
        return std::string("<UILabel id=\"") + id + "\" text=\"Common_Null\">\n"
               "        <onfocused expr=\"Enter(PauseMenu_Main_Prompts)\"></onfocused>\n"
               "        <transition event=\"@UI.ACCEPT*RELEASED\" consume=\"true\">\n"
               "          <action expr=\"PlaySound('HUD_MENU_SELECT_MASTER')\"></action>\n" + actions +
               "        </transition>\n"
               "      </UILabel>\n      ";
    };
    if (const size_t opt = s.find("<UILabel id=\"PM_Options\"", list); opt != std::string::npos) {
        s.insert(opt, item("RdmpPM_Browser",
                           "          <action expr=\"Exit(PauseMenu_Main_Prompts)\"></action>\n"
                           "          <action expr=\"unfocus(PauseMenu)\"></action>\n"
                           "          <action expr=\"Enter(RdmpBrowser)\"></action>\n"
                           "          <action expr=\"focus(RdmpBrowser)\"></action>\n") +
                      item("RdmpPM_Direct", "          <action expr=\"SendEvent('rdmp.direct')\"></action>\n"));
    }
    const std::string disconnect =
        item("RdmpPM_Disconnect", "          <action expr=\"Enter(RdmpDisconnectConf)\"></action>\n") +
        item("RdmpPM_Quit", "          <action expr=\"Enter(QuitGame)\"></action>\n");
    // Scene: our cue to set the entries before the list is entered, and Back / Pause through rdmp.back.
    if (const size_t e = s.find("expr=\"Enter(PauseMenu)\"></onfocused>"); e != std::string::npos) {
        const size_t on = s.rfind("<onfocused", e);
        if (on != std::string::npos)
            s.insert(on, "<onfocused expr=\"StreamMovie('list')\"></onfocused>\n       "
                         "<onfocused expr=\"SendEvent('rdmp.pause')\"></onfocused>\n       ");
    }
    ReplaceAfter(s, "<transition event=\"@UI.CANCEL*RELEASED\" target=\"StackPop()\"", "target=\"StackPop()\"", "target=\"SendEvent('rdmp.back')\"");
    ReplaceAfter(s, "<transition event=\"@PAUSE.PAUSE*RELEASED\" target=\"StackPop()\"", "target=\"StackPop()\"", "target=\"SendEvent('rdmp.back')\"");
    if (const size_t t = s.find("<transition event=\"rdmp.openBrowser\""); t != std::string::npos) {
        s.insert(t, "<transition event=\"rdmp.back.title\"><action expr=\"Enter(QuitGame)\"></action></transition>\n       "
                    "<transition event=\"rdmp.back.ingame\"><action expr=\"PlaySound('HUD_MENU_BACK_MASTER')\"></action>"
                    "<action expr=\"StackPop()\"></action></transition>\n       ");
    }
    // (after the inserts above: they move everything below them)
    const size_t end = FindOutsideComments(s, "</UIList>", s.find("<UIList id=\"PauseMenu\""));
    if (end == std::string::npos) return;
    const std::string afterList =
        "</UIList>\n"
        // the menu's title on the front end (the list movie's page-title slot); entered on the title only
        "    <UILabel id=\"RdmpMenuTitle\" movie=\"list\" flashObject=\"top_title\" text=\"Common_Null\"></UILabel>\n"
        "    <UILayer id=\"RdmpPauseUnused\">\n" + moved + "    </UILayer>\n"
        // Same box as the game's "return to main menu" (SG_Return2MainConf): Yes leaves the server and goes back.
        "    <UIMessageBox id=\"RdmpDisconnectConf\">\n"
        "      <data name=\"warning\"></data>\n"
        "      <data name=\"description\" expr=\"SG_Return2MainConf\"></data>\n"
        "      <UIPromptStrip>\n"
        "        <UIButton text=\"Common_Yes\" icon=\"{@UI.ACCEPT}\">\n"
        "          <transition event=\"@UI.ACCEPT*RELEASED\">\n"
        "            <action expr=\"SendEvent('rdmp.disconnect')\"></action>\n"
        "            <action expr=\"SendEvent('AwaitRebootTransition')\"></action>\n"
        "            <action expr=\"FadeOut(0.5,1.0,false)\"></action>\n"
        "            <action expr=\"Exit(RdmpDisconnectConf)\"></action>\n"
        "          </transition>\n"
        "        </UIButton>\n"
        "        <UIButton text=\"Common_No\" icon=\"{@UI.CANCEL}\">\n"
        "          <transition event=\"@UI.CANCEL*RELEASED\"><action expr=\"Exit(RdmpDisconnectConf)\"></action></transition>\n"
        "        </UIButton>\n"
        "      </UIPromptStrip>\n"
        "    </UIMessageBox>";
    s.replace(end, std::strlen("</UIList>"), disconnect + afterList);
    Log("ui files: pause menu patched (one menu)");
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
    if (_stricmp(name, "pausemenu/PauseMenuScene.sc") == 0) {
        // The server browser lives in the pause scene like the game's Options: include it, and open it on
        // "rdmp.openBrowser" (the main menu pushes the scene and sends that event).
        std::string s = ReadStream(g_open(name, a2, a3, a4));
        const char* inc = "<include src=\"Options.sc.xml\"></include>";
        const size_t at = s.find(inc);
        const char* pauseT = "<transition event=\"@PAUSE.PAUSE*RELEASED\"";
        const size_t t = s.find(pauseT);
        if (at == std::string::npos || t == std::string::npos) {
            Log("ui files: PauseMenuScene.sc anchors not found - not patched");
        } else {
            s.insert(at + std::strlen(inc), "\n    <include src=\"rdmp/Browser.sc\"></include>");
            s.insert(t,
                     "<transition event=\"rdmp.openBrowser\" consume=\"true\">"
                     "<action expr=\"Exit(PauseMenu_Main_Prompts)\"></action><action expr=\"unfocus(PauseMenu)\"></action>"
                     "<action expr=\"Enter(RdmpBrowser)\"></action><action expr=\"focus(RdmpBrowser)\"></action></transition>\n       "
                     // joining: our menus close, the world load takes over
                     "<transition event=\"rdmp.joining\" consume=\"true\"><action expr=\"Exit(RdmpBrowser)\"></action>"
                     "<action expr=\"ClearStack()\"></action></transition>\n       ");
            Log("ui files: PauseMenuScene.sc patched (server browser)");
        }
        PatchPauseMenu(s);
        return s.empty() ? nullptr : Serve(s, name, a2, a3, a4);
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
        // Global transitions (UserInterface's own see every event). rdmp.ingame (posted when a joined world is in):
        // the front end's title states exit and its movie unstreams (while it is loaded the game treats the pause menu
        // as the title's: no Map), then the HUD takes the UI stack - what HudSceneOffline's onactivate does at a normal
        // start; our front end's screens were on the stack instead, so the HUD (and its Pause handler) had no focus.
        const char* root = "<uiobject id=\"UserInterface\" class=\"UILayer\">";
        const size_t r = s.find(root);
        if (r != std::string::npos) {
            s.insert(r + std::strlen(root),
                     "\n    <transition event=\"rdmp.ingame\">"
                     "<action expr=\"Exit(StartScreen1)\"></action><action expr=\"Exit(StartScreen2)\"></action>"
                     "<action expr=\"Exit(logo)\"></action><action expr=\"Exit(background)\"></action>"
                     "<action expr=\"Exit(Startup)\"></action><action expr=\"UnstreamMovie('startscreen')\"></action>"
                     "<action expr=\"ClearStack()\"></action>"
                     "<action expr=\"StackPush_NoTransition(HudSceneOffline)\"></action></transition>");
        }
        Log("ui files: UserInterface.sc patched (rdmp/Main.sc included%s)", r != std::string::npos ? ", global transitions" : "");
        return Serve(s, name, a2, a3, a4);
    }
    if (_stricmp(name, "Boot.sc") == 0) {
        // One menu (MTA-style): the boot goes straight to StartScreen2 (the title's logo + background, keeping the title
        // movie loaded so the game treats the pause scene as the front end's: no Map) instead of the "press Play"
        // title, and StartScreen2 opens the pause scene, whose list is our menu (PatchPauseMenu). The single-player
        // mode list and StartScreen2's prompts are not entered.
        std::string s = ReadStream(g_open(name, a2, a3, a4));
        int done = 0;
        done += ReplaceAfter(s, "<transition event=\"benchmarkComplete\"", "target=\"StartScreen1\"", "target=\"StartScreen2\"");
        const char* ss2 = "<RDRUIStartScreen2 id=\"StartScreen2\"";
        done += ReplaceAfter(s, ss2, "<onactivate expr=\"Enter(MainMenuPrompts)\"       ></onactivate>",
                             "<onactivate expr=\"StackPush(PauseMenuScene)\"></onactivate>");
        done += ReplaceAfter(s, ss2, "<onactivate expr=\"Enter(StartScreen2PromptStrip)\"></onactivate>", "");
        done += ReplaceAfter(s, ss2, "<onactivate expr=\"Enter(StartScreen_NewsTicker)\"></onactivate>", "");
        done += ReplaceAfter(s, ss2, "<onactivate expr=\"Enter(StartScreen_NewsState)\" ></onactivate>", "");
        Log("ui files: Boot.sc patched (one menu: %d/5 edits)", done);
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
// g_texts holds every text we put on screen (id -> text); the map's strings are what the widgets point at.
std::map<std::string, std::string> g_texts = {
    {"RdmpMenu_Browser", "Server Browser"},
    {"RdmpMenu_Direct", "Direct Connect"},
    {"RdmpMenu_MpSettings", "Multiplayer Settings"},
    {"RdmpTab_Internet", "INTERNET"},
    {"RdmpTab_Local", "LOCAL"},
    {"RdmpTab_Favorites", "FAVORITES"},
    {"RdmpTab_Recent", "RECENT"},
    {"RdmpPrompt_Join", "Join"},
    {"RdmpPrompt_Favorite", "Favorite"},
    {"RdmpPrompt_Refresh", "Refresh"},
    {"RdmpPM_Disconnect", "DISCONNECT"},
    {"RdmpPM_Browser", "SERVER BROWSER"},
    {"RdmpPM_Direct", "DIRECT CONNECT"},
    {"RdmpPM_Quit", "QUIT"},
    {"RdmpMenuTitle", "RED DEAD MULTIPLAYER"},
};

// Server browser model (rows of ui/native/Browser.sc: RdmpSrv_<i> with RdmpSrvName_<i> / RdmpSrvInfo_<i>).
constexpr int kBrowserRows = 24;
struct BrowserRow { std::string name, info, host; int port = 0; bool favorite = false; };
std::vector<BrowserRow> g_rows;
std::string g_tab = "internet";
int g_shownRows = -1;  // rows currently visible (-1 = not applied yet)
std::map<std::string, std::vector<NetServer>> g_lists;  // answers per browser tab (from the client's server query)
bool g_searching = false;                              // the current tab was just asked, nothing back yet
ULONGLONG g_searchStart = 0;

// Browser rows: show as many as there are servers (at least one, for the "none" line), hide the rest
// (FUN_1400fe810(widget, hide) = the hide flag 0x800 + parent re-layout). Must happen before the list activates: the
// scroll list takes its items when activated (our rdmp.* events arrive synchronously, before the next action runs).
void ApplyBrowserRows() {
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
    if (!root) return;
    auto find = reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100));
    const int want = (std::max)(1, static_cast<int>(g_rows.size()));
    if (g_shownRows == want) return;
    int applied = 0;
    for (int i = 0; i < kBrowserRows; ++i) {
        const uintptr_t row = find(root, ("RdmpSrv_" + std::to_string(i)).c_str());
        if (!row) continue;
        reinterpret_cast<char (*)(uintptr_t, char)>(GameAddr(0x1400FE810))(row, i >= want ? 1 : 0);
        ++applied;
    }
    if (applied) {
        // Rows that change while the list is up: the scroll list takes its items when activated, so activate it again
        // (Deactivate vt+0x100, Activate vt+0xf8 - what the Deactivate / Activate actions call).
        if (g_shownRows >= 0) {
            if (const uintptr_t list = find(root, "RdmpServerList")) {
                const uintptr_t vt = *reinterpret_cast<uintptr_t*>(list);
                reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(vt + 0x100))(list);
                reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(vt + 0xf8))(list);
            }
        }
        g_shownRows = want;
    }
}

// The current tab's rows from the answers: online servers first (most players first), then the ones that did not
// answer (favourites / recents that are down).
void RenderBrowser() {
    std::vector<NetServer> list = g_lists[g_tab];
    std::stable_sort(list.begin(), list.end(), [](const NetServer& a, const NetServer& b) {
        if (a.online != b.online) return a.online > b.online;
        return a.players > b.players;
    });
    g_rows.clear();
    for (const NetServer& s : list) {
        if (static_cast<int>(g_rows.size()) >= kBrowserRows) break;
        if (g_tab == "local" && !s.online) continue;  // the local scan asks ports nobody serves
        BrowserRow r;
        r.host = s.host, r.port = s.port, r.favorite = s.favorite != 0;
        r.name = s.name[0] ? s.name : std::string(s.host) + ":" + std::to_string(s.port);
        if (s.favorite && g_tab != "favorites") r.name = "* " + r.name;
        char info[64];
        if (s.online) sprintf_s(info, "%s%d / %d     %d ms", s.password ? "locked   " : "", s.players, s.maxPlayers, s.pingMs);
        else strcpy_s(info, "offline");
        r.info = info;
        g_rows.push_back(r);
    }
    const char* header = g_tab == "internet" ? "INTERNET SERVERS" : g_tab == "local" ? "LOCAL SERVERS" : g_tab == "favorites" ? "FAVORITES" : "RECENT";
    g_texts["RdmpHeader"] = header;
    for (int i = 0; i < kBrowserRows; ++i) {
        g_texts["RdmpSrvName_" + std::to_string(i)] = i < static_cast<int>(g_rows.size()) ? g_rows[i].name : "";
        g_texts["RdmpSrvInfo_" + std::to_string(i)] = i < static_cast<int>(g_rows.size()) ? g_rows[i].info : "";
    }
    if (g_rows.empty()) {
        const bool searching = g_searching && GetTickCount64() - g_searchStart < 6000;
        g_texts["RdmpSrvName_0"] = searching ? "Searching for servers..." :
                                   g_tab == "favorites" ? "No favorites yet. Press the favourite key on a server to add it." :
                                   g_tab == "recent" ? "No recent servers." : "No servers found.";
    }
    ApplyBrowserRows();
}

void SetBrowserTab(const std::string& tab) {
    g_tab = tab;
    g_lists[tab].clear();
    g_searching = true;
    g_searchStart = GetTickCount64();
    NetBrowserRefresh(tab.c_str());
    RenderBrowser();
}

// Answers that arrived from the client's server query; re-renders when the current tab got some.
void PollBrowser() {
    NetServer buf[32];
    bool changed = false;
    int n;
    while ((n = NetBrowserPoll(buf, 32)) > 0) {
        for (int i = 0; i < n; ++i) {
            auto& list = g_lists[buf[i].list];
            auto it = std::find_if(list.begin(), list.end(), [&](const NetServer& s) {
                return s.port == buf[i].port && std::strcmp(s.host, buf[i].host) == 0;
            });
            if (it != list.end()) *it = buf[i];
            else list.push_back(buf[i]);
            changed |= g_tab == buf[i].list;
        }
    }
    static bool wasSearching = false;
    if (changed) g_searching = false;
    const bool searchOver = g_searching && GetTickCount64() - g_searchStart >= 6000;
    if (changed || (searchOver && !wasSearching)) RenderBrowser();
    wasSearching = searchOver;
}

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
            case 'k':  // k <input event>: raise an input event like a key press, e.g. @PAUSE.PAUSE*RELEASED (FUN_1400eeb50)
                reinterpret_cast<void (*)(uintptr_t, const char*)>(GameAddr(0x1400EEB50))(*reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8B8)), movie);
                break;
            case 'm': {  // m <ghidra address> [offset]: byte / dword at *(addr)+offset when offset given, else at addr
                const uintptr_t a = GameAddr(strtoull(movie, nullptr, 16));
                uintptr_t p = a;
                if (var[0]) {
                    p = *reinterpret_cast<uintptr_t*>(a);
                    p = p ? p + strtoull(var, nullptr, 16) : 0;
                }
                Log("ui exec: mem %s %s -> %s", movie, var, p ? (std::to_string(*reinterpret_cast<uint8_t*>(p)) + " / " + std::to_string(*reinterpret_cast<uint32_t*>(p))).c_str() : "null");
                continue;
            }
            case 'a':    // a <widget>...: is each one active (vt+0xd8(w, 1))
            case 'n': {  // n <widget>: Enter it (vt+0x160(w, 1), the Enter action)
                const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
                auto find = reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100));
                std::string names = std::string(movie) + " " + var + " " + rest;
                char report[512] = "";
                size_t pos = 0;
                while (pos < names.size()) {
                    const size_t end = names.find(' ', pos);
                    const std::string id = names.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    pos = end == std::string::npos ? names.size() : end + 1;
                    if (id.empty()) continue;
                    const uintptr_t w = root ? find(root, id.c_str()) : 0;
                    if (!w) { strcat_s(report, (id + "=? ").c_str()); continue; }
                    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(w);
                    if (kind == 'n') reinterpret_cast<void (*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(vt + 0x160))(w, 1);
                    const bool on = reinterpret_cast<bool (*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(vt + 0xd8))(w, 1);
                    strcat_s(report, (id + (on ? "=on " : "=off ")).c_str());
                }
                Log("ui exec: %s -> %s", kind == 'n' ? "enter" : "active", report);
                continue;
            }
            default: continue;
        }
        Log("ui exec: %s", line);
    }
    fclose(f);
}

}  // namespace

void UiFilesFrame(bool frontEnd) {
    if (!g_open) return;
    if (g_exec) ExecFile();
    if (frontEnd) {
        NetTitleFrame();
        PollBrowser();
    }
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
    if (!root) return;
    auto find = reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100));
    const uintptr_t textVt = GameAddr(0x141C61950);
    const uintptr_t buttonVt = GameAddr(0x141C64F78);  // UIButton: its text is the label at +0x90
    for (const auto& [id, text] : g_texts) {
        uintptr_t w = find(root, id.c_str());
        if (w && *reinterpret_cast<uintptr_t*>(w) == buttonVt) w = *reinterpret_cast<uintptr_t*>(w + 0x90);
        if (!w || *reinterpret_cast<uintptr_t*>(w) != textVt) continue;
        if (*reinterpret_cast<uint8_t*>(w + 0x80) == 0 && *reinterpret_cast<const char**>(w + 0x88) == text.c_str()) continue;
        *reinterpret_cast<uint8_t*>(w + 0x80) = 0;
        *reinterpret_cast<const char**>(w + 0x88) = text.c_str();
        reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(w) + 0x3d0))(w);
    }
    ApplyBrowserRows();
}

void UiFilesOnEvent(const char* name) {
    if (!g_open || !name || std::strncmp(name, "rdmp.", 5) != 0) return;
    const std::string ev = name + 5;
    if (ev.rfind("tab.", 0) == 0) {
        SetBrowserTab(ev.substr(4));
    } else if (ev == "browser.open" || ev == "refresh") {
        SetBrowserTab(g_tab);
    } else if (ev.rfind("join.", 0) == 0) {
        const int i = atoi(ev.c_str() + 5);
        if (i < 0 || i >= static_cast<int>(g_rows.size()) || g_rows[i].host.empty()) return;
        Log("ui: join %s:%d (%s)", g_rows[i].host.c_str(), g_rows[i].port, g_rows[i].name.c_str());
        NetBrowserJoin(g_rows[i].host.c_str(), g_rows[i].port);
        PostUiEvent("rdmp.joining");  // our menus close (PauseMenuScene.sc patch), the world load takes over
    } else if (ev == "pause") {
        // The one menu opens (title or pause): show the entries that fit, before the list is entered.
        const bool frontEnd = !GameflowWorldLoadStarted();
        const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
        auto find = reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100));
        auto setHidden = reinterpret_cast<char (*)(uintptr_t, char)>(GameAddr(0x1400FE810));
        const struct { const char* id; bool hide; } kItems[] = {
            {"RdmpPM_Browser", !frontEnd}, {"RdmpPM_Direct", !frontEnd}, {"RdmpPM_Disconnect", frontEnd},
            // the pause screen's pocket watch and money mean nothing on the title
            {"clock", frontEnd}, {"pauseMoney", frontEnd}, {"pauseMoneyValue", frontEnd}};
        for (const auto& it : kItems)
            if (const uintptr_t w = root ? find(root, it.id) : 0) setHidden(w, it.hide ? 1 : 0);
        // RED DEAD MULTIPLAYER over the menu on the title (Enter = vt+0x160(w, 1), Exit = vt+0x160(w, 0))
        if (const uintptr_t t = root ? find(root, "RdmpMenuTitle") : 0)
            reinterpret_cast<void (*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(t) + 0x160))(t, frontEnd ? 1 : 0);
    } else if (ev == "back") {
        PostUiEvent(GameflowWorldLoadStarted() ? "rdmp.back.ingame" : "rdmp.back.title");
    } else if (ev == "direct") {
        Log("ui: direct connect (not built yet)");
    } else if (ev == "disconnect") {
        NetDisconnect();
    } else if (ev.rfind("favorite.", 0) == 0) {
        const int i = atoi(ev.c_str() + 9);
        if (i < 0 || i >= static_cast<int>(g_rows.size()) || g_rows[i].host.empty()) return;
        NetBrowserFavorite(g_rows[i].host.c_str(), g_rows[i].port, g_rows[i].name.c_str(), !g_rows[i].favorite);
        Log("ui: %s favourites: %s:%d", g_rows[i].favorite ? "removed from" : "added to", g_rows[i].host.c_str(), g_rows[i].port);
        SetBrowserTab(g_tab);
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
