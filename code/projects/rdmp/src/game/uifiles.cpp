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
#include <cstdlib>
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
// root "UserInterface" widget when one is missing, so they must exist), and our entries are added.
void PatchPauseMenu(std::string& s) {
    const size_t list = s.find("<UIList id=\"PauseMenu\"");
    if (list == std::string::npos) {
        Log("ui files: pause menu list not found - not patched");
        return;
    }
    std::string moved;  // (PM_Network, PM_SocialClub and PM_Leaderboards are commented out in the game's file)
    for (const char* id : {"PM_Map", "PM_Weapons", "PM_Outfits", "PM_Journal", "PM_Stats", "PM_Load"}) {
        const std::string open = std::string("<UILabel id=\"") + id + "\"";
        const size_t a = s.find(open, list);
        const size_t b = a == std::string::npos ? a : s.find("</UILabel>", a);
        if (b == std::string::npos) continue;
        moved += "      " + s.substr(a, b + std::strlen("</UILabel>") - a) + "\n";
        s.erase(a, b + std::strlen("</UILabel>") - a);
    }
    // The one menu (MTA-style, title and pause): Resume (on a server), Server Browser, Direct Connect (off a
    // server), Options, Disconnect (on a server), Quit. Which ones show is set on rdmp.pause (UiFilesOnEvent) before the
    // list is entered, and again when Disconnect leaves; Back goes through rdmp.back (quit prompt off a server, close on
    // one).
    auto item = [](const char* id, const char* actions) {
        return std::string("<UILabel id=\"") + id + "\" text=\"Common_Null\">\n"
               "        <onfocused expr=\"Enter(PauseMenu_Main_Prompts)\"></onfocused>\n"
               "        <transition event=\"@UI.ACCEPT*RELEASED\" consume=\"true\">\n"
               "          <action expr=\"PlaySound('HUD_MENU_SELECT_MASTER')\"></action>\n" + actions +
               "        </transition>\n"
               "      </UILabel>\n      ";
    };
    // Resume first: before the first entry left in the list
    if (const size_t first = s.find("<UILabel id=\"", list); first != std::string::npos)
        s.insert(first, item("RdmpPM_Resume", "          <action expr=\"StackPop()\"></action>\n"));
    if (const size_t opt = s.find("<UILabel id=\"PM_Options\"", list); opt != std::string::npos) {
        s.insert(opt, item("RdmpPM_Browser",
                           "          <action expr=\"Exit(PauseMenu_Main_Prompts)\"></action>\n"
                           "          <action expr=\"SendEvent('rdmp.view.RdmpPM_Browser')\"></action>\n"
                           "          <action expr=\"unfocus(PauseMenu)\"></action>\n"
                           "          <action expr=\"Enter(RdmpBrowser)\"></action>\n"
                           "          <action expr=\"focus(RdmpBrowser)\"></action>\n") +
                      item("RdmpPM_Direct", "          <action expr=\"SendEvent('rdmp.edit.direct')\"></action>\n"
                                            "          <action expr=\"Enter(RdmpTextEntry)\"></action>\n"));
    }
    // Each entry opens its own view: the pause scene draws a view's category list as the submenu of a row of this list
    // (flashObject menu.elements[i]); the mod hides every other entry (rdmp.view.<entry>), so the chosen one is row 0 and
    // heads its view, and Back shows them again (rdmp.view.main).
    ReplaceAfter(s, "<UILabel id=\"PM_Options\"", "<action expr=\"unfocus(PauseMenu)\"></action>",
                 "<action expr=\"SendEvent('rdmp.view.PM_Options')\"></action><action expr=\"unfocus(PauseMenu)\"></action>");
    const std::string disconnect =
        // Disconnect leaves at once: the world stays, the player goes back to limbo and this menu becomes the main menu
        // (the scene builds the menu column when it opens, from the entries shown then: it closes here and opens again as
        // the main menu, rdmp.menu posted by the mod)
        item("RdmpPM_Disconnect", "          <action expr=\"SendEvent('rdmp.disconnect')\"></action>\n"
                                  "          <action expr=\"StackPop()\"></action>\n") +
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
        // Text entry (Direct Connect address, nickname): the game's cheat entry box (popup_cheatInput). Its description
        // is the game's dynamic text Enter_Cheat_Dialog, which the mod formats with what is typed (TextEntryFrame).
        "    <UIMessageBox id=\"RdmpTextEntry\" movie=\"popup\">\n"
        "      <data name=\"title\" expr=\"Common_Null\"></data>\n"
        "      <data name=\"description\" expr=\"Enter_Cheat_Dialog\"></data>\n"
        "      <transition event=\"@UI.ACCEPT*RELEASED\" consume=\"true\">\n"
        "        <action expr=\"PlaySound('HUD_MENU_SELECT_MASTER')\"></action>\n"
        "        <action expr=\"Exit(RdmpTextEntry)\"></action>\n"
        "        <action expr=\"SendEvent('rdmp.text.ok')\"></action>\n"
        "      </transition>\n"
        "      <transition event=\"@UI.VKB_CANCEL*RELEASED\" consume=\"true\">\n"
        "        <action expr=\"PlaySound('HUD_MENU_BACK_MASTER')\"></action>\n"
        "        <action expr=\"Exit(RdmpTextEntry)\"></action>\n"
        "        <action expr=\"SendEvent('rdmp.text.cancel')\"></action>\n"
        "      </transition>\n"
        "      <UIPromptStrip>\n"
        "        <UIButton text=\"Common_Ok\" icon=\"{@UI.ACCEPT}\"></UIButton>\n"
        "        <UIButton text=\"Common_Cancel\" icon=\"{@UI.VKB_CANCEL}\"></UIButton>\n"
        "      </UIPromptStrip>\n"
        "    </UIMessageBox>";
    s.replace(end, std::strlen("</UIList>"), disconnect + afterList);
    Log("ui files: pause menu patched (one menu)");
}

// Nametag distances offered in the MULTIPLAYER options (metres; [client] nametag_distance, the HTML menu allowed 5..500).
constexpr int kNametagDistances[] = {20, 40, 60, 80, 100, 150, 200, 300, 500};

// The game's Options (Options.sc.xml, included by the pause scene) for multiplayer:
//   * the single-player tabs move out of the tab list into a container that is never entered (kept, never deleted: the
//     game's C++ finds their widgets by name). GAMEPLAY holds Golden Guns and Autosave (single player) and the
//     multiplayer targeting mode, which the game only reads in its own online sessions (*(0x142C456D0)+0x12E bit 8,
//     FUN_1406262b0), never in ours, and shows only there; EXTRAS is the cheats list.
//   * a MULTIPLAYER tab (first) with nickname, nametags, nametag distance and chat ([net] name, [client] nametags /
//     nametag_distance / chat in rdmp.ini, through the client DLL: the same settings the HTML menu had).
//   * Back enters the main list again (it left when Options opened, PatchPauseMenu).
void PatchOptions(std::string& s) {
    const size_t tabs = s.find("<UIList id=\"OptionsCatagoriesTabs\"");
    if (tabs == std::string::npos) {
        Log("ui files: Options tab list not found - not patched");
        return;
    }
    std::string moved;
    for (const char* id : {"<UILabel id =\"OptionsTab_Controls\"", "<UILabel id=\"PM_Extras\""}) {
        const size_t a = FindOutsideComments(s, id, tabs);
        const size_t b = a == std::string::npos ? a : s.find("</UILabel>", a);
        if (b == std::string::npos) {
            Log("ui files: Options tab %s not found", id);
            continue;
        }
        moved += "      " + s.substr(a, b + std::strlen("</UILabel>") - a) + "\n";
        s.erase(a, b + std::strlen("</UILabel>") - a);
    }
    // The tabs are the submenu of a row of the main list: the game's OPTIONS was row 5 (menu.elements[5]); ours is row 0
    // of its view (the other entries hidden, PatchPauseMenu).
    ReplaceAfter(s, "<UIList id=\"OptionsCatagoriesTabs\"", "flashObject=\"menu.elements[5]\"", "flashObject=\"menu.elements[0]\"");
    // Our list leaves when another tab shows (the game's lists exit each other in their onshow).
    for (const char* id : {"\"OptionsTab_Gamepad\"", "\"OptionsTab_KBM\"", "\"OptionsTab_Audio\"", "\"OptionsTab_Display\"",
                           "\"OptionsTab_Graphics\"", "\"OptionsTab_Legal\""}) {
        const size_t a = s.find(id, tabs);
        const size_t gt = a == std::string::npos ? a : s.find('>', a);
        if (gt != std::string::npos) s.insert(gt + 1, "\n          <onfocused expr=\"Exit(RdmpOptionsList_MP)\"></onfocused>");
    }
    const std::string ourTab =
        "\n        <UILabel id=\"RdmpOptionsTab_MP\" text=\"Common_Null\">\n"
        "          <onfocused expr=\"SendEvent('rdmp.opt.load')\"></onfocused>\n"
        "          <onfocused expr=\"Include(omp_action)\"></onfocused>\n"
        "          <onfocused expr=\"Exclude(omp_enter)\"></onfocused>\n"
        "          <onfocused expr=\"activate(RdmpOptionsList_MP)\"></onfocused>\n"
        "          <onfocused expr=\"activate(Options_arrows)\"></onfocused>\n"
        "          <onfocused expr=\"show(Options_arrows)\"></onfocused>\n"
        "          <onfocused expr=\"show(RdmpOptionsList_MP)\"></onfocused>\n"
        "          <onunfocused expr=\"Exit(Options_Main_Prompts)\"></onunfocused>\n"
        "          <onfocused expr=\"Enter(Options_Main_Prompts)\"></onfocused>\n"
        "          <transition event=\"@UI.ACCEPT*RELEASED\" consume=\"true\">\n"
        "            <action expr=\"focus(RdmpOptionsList_MP)\"></action>\n"
        "            <action expr=\"Unfocus(OptionsCatagoriesTabs)\"></action>\n"
        "            <action expr=\"Exit(Options_Main_Prompts)\"></action>\n"
        "            <action expr=\"Enter(Options_Prompts)\"></action>\n"
        "            <action expr=\"SendEvent('AUDIO_StartMenuItemSelected')\"></action>\n"
        "          </transition>\n"
        "        </UILabel>\n";
    if (const size_t gt = s.find('>', s.find("<UIList id=\"OptionsCatagoriesTabs\"")); gt != std::string::npos) s.insert(gt + 1, ourTab);
    // the never-entered container, after the tab list
    if (const size_t end = FindOutsideComments(s, "</UIList>", s.find("<UIList id=\"OptionsCatagoriesTabs\"")); end != std::string::npos)
        s.insert(end + std::strlen("</UIList>"), "\n    <UILayer id=\"RdmpOptionsUnused\">\n" + moved + "    </UILayer>");
    // The MULTIPLAYER rows: a label and a UIOption each, like the game's rows. Labels and values without a string-table
    // key are set by id (g_texts); values are read and written by the mod on rdmp.opt.load / rdmp.opt.changed.
    auto option = [](const char* id, const std::string& values) {
        return std::string("      <UIList horizontal=\"true\" scrollable=\"false\" allowInput=\"false\">\n"
                           "        <onfocused expr=\"focus(") + id + ")\"></onfocused>\n"
               "        <UILabel id=\"" + id + "_Label\" text=\"Common_Null\"></UILabel>\n"
               "        <UIOption id=\"" + id + "\" allowWrap=\"true\">\n"
               "          <onchanged expr=\"SendEvent('rdmp.opt.changed')\"></onchanged>\n" + values +
               "        </UIOption>\n"
               "      </UIList>\n";
    };
    const std::string offOn = "          <UILabel text=\"Common_Off\"></UILabel>\n          <UILabel text=\"Common_On\"></UILabel>\n";
    std::string distances;
    for (int i = 0; i < static_cast<int>(std::size(kNametagDistances)); ++i)
        distances += "          <UILabel id=\"RdmpOpt_Distance_" + std::to_string(i) + "\" text=\"Common_Null\"></UILabel>\n";
    std::string list =
        "<UIScrollableList id=\"RdmpOptionsList_MP\" MaxHeight=\"13\" vertical=\"true\" scrollable=\"true\" movie=\"list\" consume=\"true\" allowScrollbar=\"true\" allowMouseAutoSelect=\"true\">\n";
    for (const char* other : {"OptionsList_Controls", "OptionsList_Audio", "OptionsList_Display", "OptionsList_Graphics", "OptionsList_Legal",
                              "OptionsList_Gamepad", "OptionsList_KBM", "ExtrasLayer"})
        list += std::string("      <onshow expr=\"Exit(") + other + ")\"></onshow>\n";
    list +=
        "      <transition event=\"@UI.CANCEL*RELEASED\" consume=\"true\">\n"
        "        <action expr=\"PlaySound('HUD_MENU_BACK_MASTER')\"></action>\n"
        "        <action expr=\"unfocus(RdmpOptionsList_MP)\"></action>\n"
        "        <action expr=\"Focus(OptionsCatagoriesTabs)\"></action>\n"
        "        <action expr=\"Enter(Options_Main_Prompts)\"></action>\n"
        "        <action expr=\"Exit(Options_Prompts)\"></action>\n"
        "      </transition>\n"
        // Nickname: Accept opens the text entry
        "      <UIList horizontal=\"true\" scrollable=\"false\" allowInput=\"false\">\n"
        "        <onfocused expr=\"focus(RdmpOpt_Nick)\"></onfocused>\n"
        "        <UILabel id=\"RdmpOpt_Nick_Label\" text=\"Common_Null\"></UILabel>\n"
        "        <UIOption id=\"RdmpOpt_Nick\" allowWrap=\"false\">\n"
        "          <transition event=\"@UI.ACCEPT*RELEASED\" consume=\"true\">\n"
        "            <action expr=\"PlaySound('HUD_MENU_SELECT_MASTER')\"></action>\n"
        "            <action expr=\"SendEvent('rdmp.edit.nick')\"></action>\n"
        "            <action expr=\"Enter(RdmpTextEntry)\"></action>\n"
        "          </transition>\n"
        "          <UILabel id=\"RdmpOpt_Nick_Value\" text=\"Common_Null\"></UILabel>\n"
        "        </UIOption>\n"
        "      </UIList>\n" +
        option("RdmpOpt_Nametags", offOn) + option("RdmpOpt_Distance", distances) + option("RdmpOpt_Chat", offOn) +
        "    </UIScrollableList>\n    ";
    s.insert(s.find("<UIList id=\"OptionsCatagoriesTabs\""), list);
    // Options' own Back and focus loss
    ReplaceAfter(s, "<rdrOptionsLayer id=\"OptionsLayer\">", "<action expr=\"focus(PauseMenu)\"></action>",
                 "<action expr=\"SendEvent('rdmp.view.main')\"></action><action expr=\"focus(PauseMenu)\"></action>");
    ReplaceAfter(s, "<rdrOptionsLayer id=\"OptionsLayer\">", "<onunfocused expr=\"Unfocus(ExtrasLayer)\"></onunfocused>",
                 "<onunfocused expr=\"Unfocus(ExtrasLayer)\"></onunfocused>\n    <onunfocused expr=\"Unfocus(RdmpOptionsList_MP)\"></onunfocused>");
    Log("ui files: Options patched (MULTIPLAYER tab, single-player tabs out)");
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
                     "<action expr=\"Exit(PauseMenu_Main_Prompts)\"></action><action expr=\"SendEvent('rdmp.view.RdmpPM_Browser')\"></action>"
                     "<action expr=\"unfocus(PauseMenu)\"></action>"
                     "<action expr=\"Enter(RdmpBrowser)\"></action><action expr=\"focus(RdmpBrowser)\"></action></transition>\n       "
                     // joining: our menus close, the world load takes over
                     "<transition event=\"rdmp.joining\" consume=\"true\"><action expr=\"Exit(RdmpBrowser)\"></action>"
                     "<action expr=\"ClearStack()\"></action></transition>\n       ");
            Log("ui files: PauseMenuScene.sc patched (server browser)");
        }
        PatchPauseMenu(s);
        return s.empty() ? nullptr : Serve(s, name, a2, a3, a4);
    }
    if (_stricmp(name, "Options.sc.xml") == 0) {
        std::string s = ReadStream(g_open(name, a2, a3, a4));
        PatchOptions(s);
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
                     "<action expr=\"StackPush_NoTransition(HudSceneOffline)\"></action></transition>"
                     // rdmp.menu (dropped by the server): the main menu opens over the world, as the pause key does
                     "\n    <transition event=\"rdmp.menu\"><action expr=\"StackPush(PauseMenuScene)\"></action></transition>");
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
    {"RdmpPM_Resume", "RESUME"},
    {"RdmpPM_Quit", "QUIT"},
    {"RdmpMenuTitle", "RED DEAD MULTIPLAYER"},
    {"RdmpOptionsTab_MP", "MULTIPLAYER"},
    {"RdmpOpt_Nick_Label", "Nickname"},
    {"RdmpOpt_Nametags_Label", "Nametags"},
    {"RdmpOpt_Distance_Label", "Nametag Distance"},
    {"RdmpOpt_Chat_Label", "Chat"},
};

// A widget by id (FUN_1400ef100 on the UI root), 0 when the UI or the widget is not there.
uintptr_t Widget(const char* id) {
    const uintptr_t root = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AAC8C0));
    return root ? reinterpret_cast<uintptr_t (*)(uintptr_t, const char*)>(GameAddr(0x1400EF100))(root, id) : 0;
}
uintptr_t VFunc(uintptr_t object, size_t offset) {
    return *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(object) + offset);
}
// UIOption value (what the game's Options read and write: vt+0x410(w, 0) = current index, vt+0x468(w, index, 0) sets it;
// FUN_140626c10 / FUN_1406262b0).
int OptionValue(const char* id) {
    const uintptr_t w = Widget(id);
    return w ? reinterpret_cast<int (*)(uintptr_t, int)>(VFunc(w, 0x410))(w, 0) : 0;
}
void SetOptionValue(const char* id, int value) {
    if (const uintptr_t w = Widget(id)) reinterpret_cast<void (*)(uintptr_t, int, int)>(VFunc(w, 0x468))(w, value, 0);
}

// ---- Text entry: the game's own keyboard text input (the Extras tab's "enter cheat" path) ----
// FUN_140623270 (ExtrasLayer.OnKeyboardActivate) starts it, FUN_140622a00 (per frame) shows the text, FUN_1406234a0
// (UI_CheatEntered / UI_CheatCancelled) ends it:
//   * virtual keyboard K = *(0x142AC4748) + 0x68: +0x160 initial text (wchar), +0x1c0 result (wchar), +0x220 / +0x224
//     title / description text hashes, +0x228 state (0 open, 1 accepted, 2 cancelled), +0x22c open, +0x230 platform
//     keyboard (vt+0 show(K + 0x270), vt+8 close), +0x280 / +0x2b8 / +0x2f0 title / initial / description strings
//     (FUN_140eab570 assign, FUN_140ebd6b0 finish), +0x328 maximum length.
//   * typed text T = *(*(0x142893518) + 0x68) (the input manager's text buffer): +0 std::wstring text, +0x20 allowed
//     characters, +0x40 (cleared), +0x60 maximum length; wstring assign = FUN_1401fc820(str, wchar*, length).
//   * the box shows the game's dynamic text Enter_Cheat_Dialog, formatted with FUN_1400fb250(textMgr, key, fmt, ...).
struct TextEntry {
    std::string purpose;  // "nick", "direct"
    std::string label;
    std::string shown;    // the text last put in the box
    bool open = false;
} g_entry;
std::string g_lastAddress;  // the last Direct Connect address (offered again)
ULONGLONG g_menuAtMs = 0;   // when to open the main menu again after a Disconnect (0 = not pending)
bool g_inView = false;      // a view (Options, server browser) is open from the main list (rdmp.view.*)

// A widget is active (entered): vt+0xd8(w, 1).
bool Active(const char* id) {
    const uintptr_t w = Widget(id);
    return w && reinterpret_cast<bool (*)(uintptr_t, int)>(VFunc(w, 0xd8))(w, 1);
}

uint32_t TextHash(const char* key) {
    const uint32_t h = reinterpret_cast<uint32_t (*)(const char*, int)>(GameAddr(0x1400C7510))(key, 0);
    return ((h * 9) >> 11 ^ h * 9) * 0x8001;
}
uintptr_t Keyboard() {
    const uintptr_t sys = *reinterpret_cast<uintptr_t*>(GameAddr(0x142AC4748));
    return sys ? sys + 0x68 : 0;
}
uintptr_t TypedText() {
    const uintptr_t input = *reinterpret_cast<uintptr_t*>(GameAddr(0x142893518));
    return input ? *reinterpret_cast<uintptr_t*>(input + 0x68) : 0;
}
std::string ReadTyped() {
    const uintptr_t t = TypedText();
    if (!t) return {};
    const size_t size = *reinterpret_cast<size_t*>(t + 0x10);
    const size_t cap = *reinterpret_cast<size_t*>(t + 0x18);
    const wchar_t* w = cap >= 8 ? *reinterpret_cast<const wchar_t**>(t) : reinterpret_cast<const wchar_t*>(t);
    if (!size || !w) return {};
    char utf8[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>((std::min)(size, size_t{100})), utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return std::string(utf8, n > 0 ? n : 0);
}
void ShowTyped(const std::string& text) {
    g_entry.shown = text;
    const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(GameAddr(0x142C45608));
    if (!mgr) return;
    reinterpret_cast<void (*)(uintptr_t, const char*, const char*, ...)>(GameAddr(0x1400FB250))(
        mgr, "Enter_Cheat_Dialog", "%s:\n<0xffff00>%s</0x>_", g_entry.label.c_str(), text.c_str());
    if (const uintptr_t box = Widget("RdmpTextEntry")) reinterpret_cast<void (*)(uintptr_t)>(VFunc(box, 0x3d0))(box);
}

bool TextEntryBegin(const char* purpose, const char* label, const std::string& initial, int maxLength) {
    const uintptr_t k = Keyboard(), t = TypedText();
    const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(GameAddr(0x142C45608));
    if (!k || !t || !mgr) {
        Log("ui: text entry not available (keyboard %p, text %p)", reinterpret_cast<void*>(k), reinterpret_cast<void*>(t));
        return false;
    }
    g_entry.purpose = purpose;
    g_entry.label = label;
    if (!*reinterpret_cast<uint8_t*>(k + 0x22c)) {  // (what FUN_140623270 does; skipped while one is open)
        auto text = reinterpret_cast<uintptr_t (*)(uintptr_t, uint32_t)>(GameAddr(0x1400FB020));
        auto assign = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(GameAddr(0x140EAB570));
        auto finish = reinterpret_cast<void (*)(uintptr_t)>(GameAddr(0x140EBD6B0));
        *reinterpret_cast<uint16_t*>(k + 0x160) = 0;
        *reinterpret_cast<uint32_t*>(k + 0x220) = TextHash("Common_Null");
        *reinterpret_cast<uint32_t*>(k + 0x224) = TextHash("Common_Null");
        *reinterpret_cast<uint16_t*>(k + 0x1c0) = 0;
        *reinterpret_cast<uint32_t*>(k + 0x228) = 0;
        assign(k + 0x280, text(mgr, *reinterpret_cast<uint32_t*>(k + 0x220)));
        finish(k + 0x280);
        assign(k + 0x2b8, k + 0x160);
        finish(k + 0x2b8);
        assign(k + 0x2f0, text(mgr, *reinterpret_cast<uint32_t*>(k + 0x224)));
        finish(k + 0x2f0);
        *reinterpret_cast<uint32_t*>(k + 0x328) = static_cast<uint32_t>(maxLength);
        const uintptr_t platform = *reinterpret_cast<uintptr_t*>(k + 0x230);
        reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(VFunc(platform, 0))(platform, k + 0x270);
        *reinterpret_cast<uint8_t*>(k + 0x22c) = 1;
    }
    auto wassign = reinterpret_cast<void (*)(uintptr_t, const wchar_t*, size_t)>(GameAddr(0x1401FC820));
    static const wchar_t kAllowed[] = L"0123456789 abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ`!\"$%^&*()-_=+[];:'@#~,./\\?|";
    wchar_t w[128] = L"";
    MultiByteToWideChar(CP_UTF8, 0, initial.c_str(), -1, w, 127);
    wassign(t, w, wcslen(w));
    *reinterpret_cast<int32_t*>(t + 0x60) = maxLength;
    wassign(t + 0x40, L"", 0);
    wassign(t + 0x20, kAllowed, wcslen(kAllowed));
    g_entry.open = true;
    ShowTyped(initial);
    Log("ui: text entry \"%s\" opened", label);
    return true;
}

// Closes the text entry (accepted or not, FUN_1406234a0); returns what was typed.
std::string TextEntryEnd(bool accepted) {
    const std::string typed = ReadTyped();
    g_entry.open = false;
    if (const uintptr_t k = Keyboard(); k && *reinterpret_cast<uint8_t*>(k + 0x22c)) {
        *reinterpret_cast<uint32_t*>(k + 0x228) = accepted ? 1 : 2;
        *reinterpret_cast<uint8_t*>(k + 0x22c) = 0;
        const uintptr_t platform = *reinterpret_cast<uintptr_t*>(k + 0x230);
        reinterpret_cast<void (*)(uintptr_t)>(VFunc(platform, 8))(platform);
    }
    return typed;
}

void TextEntryFrame() {
    if (!g_entry.open) return;
    const std::string typed = ReadTyped();
    if (typed != g_entry.shown) ShowTyped(typed);
}

// MULTIPLAYER options: the values from the client DLL into the rows, and back.
NetSettings g_settings{};
bool g_loadingMpOptions = false;  // setting a UIOption raises its onchanged (rdmp.opt.changed): not a change by the player
std::string g_mpHeader = "MULTIPLAYER";
void LoadMpOptions() {
    // Options' header (Options_Header, a text widget the game's tabs set by key with SetTextCB): our literal text.
    if (const uintptr_t h = Widget("Options_Header")) {
        *reinterpret_cast<uint8_t*>(h + 0x80) = 0;
        *reinterpret_cast<const char**>(h + 0x88) = g_mpHeader.c_str();
        reinterpret_cast<void (*)(uintptr_t)>(VFunc(h, 0x3d0))(h);
    }
    if (!NetGetSettings(&g_settings)) {
        Log("ui: multiplayer settings not available (client not up)");
        return;
    }
    g_loadingMpOptions = true;
    g_texts["RdmpOpt_Nick_Value"] = g_settings.nickname;
    SetOptionValue("RdmpOpt_Nametags", g_settings.nametags ? 1 : 0);
    int best = 0;
    for (int i = 0; i < static_cast<int>(std::size(kNametagDistances)); ++i)
        if (std::abs(kNametagDistances[i] - g_settings.nametagDistance) < std::abs(kNametagDistances[best] - g_settings.nametagDistance)) best = i;
    SetOptionValue("RdmpOpt_Distance", best);
    SetOptionValue("RdmpOpt_Chat", g_settings.chat ? 1 : 0);
    g_loadingMpOptions = false;
}
void SaveMpOptions() {
    if (g_loadingMpOptions) return;
    g_settings.nametags = OptionValue("RdmpOpt_Nametags") ? 1 : 0;
    const int d = OptionValue("RdmpOpt_Distance");
    g_settings.nametagDistance = kNametagDistances[(std::clamp)(d, 0, static_cast<int>(std::size(kNametagDistances)) - 1)];
    g_settings.chat = OptionValue("RdmpOpt_Chat") ? 1 : 0;
    NetSetSettings(g_settings);
    NetGetSettings(&g_settings);  // (the nickname as cleaned)
    g_texts["RdmpOpt_Nick_Value"] = g_settings.nickname;
    Log("ui: multiplayer settings saved (name \"%s\", nametags %d at %d m, chat %d)", g_settings.nickname, g_settings.nametags,
        g_settings.nametagDistance, g_settings.chat);
}

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
            case 'q': {  // q <ghidra address> [off]...: follow a pointer chain (*addr, then *(p+off) per offset), log each
                uintptr_t p = *reinterpret_cast<uintptr_t*>(GameAddr(strtoull(movie, nullptr, 16)));
                std::string out = std::to_string(p);
                char hex[32];
                sprintf_s(hex, "%llx", static_cast<unsigned long long>(p));
                out = hex;
                std::string offs = std::string(var) + " " + rest;
                size_t at = 0;
                while (p && at < offs.size()) {
                    const size_t end = offs.find(' ', at);
                    const std::string o = offs.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    at = end == std::string::npos ? offs.size() : end + 1;
                    if (o.empty()) continue;
                    p = *reinterpret_cast<uintptr_t*>(p + strtoull(o.c_str(), nullptr, 16));
                    sprintf_s(hex, " -[+%s]-> %llx", o.c_str(), static_cast<unsigned long long>(p));
                    out += hex;
                }
                Log("ui exec: chain %s: %s", movie, out.c_str());
                continue;
            }
            case 'h': {  // h <widget> <0|1>: show / hide it (FUN_1400fe810)
                if (const uintptr_t w = Widget(movie))
                    reinterpret_cast<char (*)(uintptr_t, char)>(GameAddr(0x1400FE810))(w, atoi(var) ? 1 : 0);
                break;
            }
            case 'a':    // a <widget>...: is each one active (vt+0xd8(w, 1))
            case 'n':    // n <widget>: Enter it (vt+0x160(w, 1), the Enter action)
            case 'x': {  // x <widget>: Exit it (vt+0x160(w, 0))
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
                    if (kind == 'n' || kind == 'x') reinterpret_cast<void (*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(vt + 0x160))(w, kind == 'n' ? 1 : 0);
                    const bool on = reinterpret_cast<bool (*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(vt + 0xd8))(w, 1);
                    const bool hidden = (*reinterpret_cast<uint16_t*>(w + 0x28) & 0x800) != 0;  // FUN_1400fe810's flag
                    strcat_s(report, (id + (on ? "=on" : "=off") + (hidden ? "(hidden) " : " ")).c_str());
                }
                Log("ui exec: %s -> %s", kind == 'n' ? "enter" : kind == 'x' ? "exit" : "active", report);
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
    if (g_menuAtMs && GetTickCount64() >= g_menuAtMs) {
        g_menuAtMs = 0;
        PostUiEvent("rdmp.menu");
    }
    if (frontEnd) NetTitleFrame();  // (with a world loaded the client is pumped by the net frame)
    PollBrowser();                  // (the browser also works in game, after a Disconnect)
    TextEntryFrame();
    // The logo and the latest changes (client overlay) while the bare main list is on screen off a server: not in a view,
    // not under the text entry or the quit prompt, not while joining (the scene closes).
    const bool overlay = !NetOnServer() && Active("PauseMenu") && !g_inView && !g_entry.open && !Active("QuitGame");
    NetSetMenuOverlay(overlay);  // (every frame: the client may come up after the menu)
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

namespace {
// The main list's entries for where we are: on a server (Resume, Options, Disconnect, Quit) or not (Server Browser,
// Direct Connect, Options, Quit), also with a world loaded behind the menu after a Disconnect. FUN_1400fe810(widget, hide).
void ApplyMainEntries() {
    const bool onServer = NetOnServer();
    auto setHidden = reinterpret_cast<char (*)(uintptr_t, char)>(GameAddr(0x1400FE810));
    const struct { const char* id; bool show; } kItems[] = {
        {"RdmpPM_Resume", onServer}, {"RdmpPM_Browser", !onServer}, {"RdmpPM_Direct", !onServer},
        {"RdmpPM_Disconnect", onServer}, {"PM_Options", true}, {"RdmpPM_Quit", true},
        // the pause screen's pocket watch and money mean nothing off a server
        {"clock", onServer}, {"pauseMoney", onServer}, {"pauseMoneyValue", onServer}};
    for (const auto& it : kItems)
        if (const uintptr_t w = Widget(it.id)) setHidden(w, it.show ? 0 : 1);
    g_inView = false;  // (a join from inside a view leaves without rdmp.view.main)
    // RED DEAD MULTIPLAYER over the menu off a server (Enter = vt+0x160(w, 1), Exit = vt+0x160(w, 0))
    if (const uintptr_t t = Widget("RdmpMenuTitle")) reinterpret_cast<void (*)(uintptr_t, int)>(VFunc(t, 0x160))(t, onServer ? 0 : 1);
}

// A join started from the menus: they close (rdmp.joining, PauseMenuScene.sc patch). With the world still loaded (after a
// Disconnect) no world load follows, so the HUD takes the UI stack now (rdmp.ingame, as when a world load finishes).
void JoinStarted() {
    PostUiEvent("rdmp.joining");
    if (GameflowWorldLoadStarted()) PostUiEvent("rdmp.ingame");
}
}  // namespace

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
        JoinStarted();  // our menus close (PauseMenuScene.sc patch), the world load takes over
    } else if (ev == "pause") {
        ApplyMainEntries();  // the one menu opens (title or pause): the entries that fit, before the list is entered
    } else if (ev.rfind("view.", 0) == 0) {
        // A view opens from the main list (rdmp.view.<entry id>) or closes (rdmp.view.main). The pause scene draws a
        // view's category list as the submenu of a row of the main list (menu.elements[0]): while it is open, every other
        // entry is hidden, so its own entry is row 0 and heads the view. Closing restores what was shown (hidden = widget
        // +0x28 flag 0x800).
        static const char* const kEntries[] = {"RdmpPM_Resume", "RdmpPM_Browser", "RdmpPM_Direct", "PM_Options", "RdmpPM_Disconnect", "RdmpPM_Quit"};
        static bool shown[std::size(kEntries)] = {};
        auto setHidden = reinterpret_cast<char (*)(uintptr_t, char)>(GameAddr(0x1400FE810));
        const std::string entry = ev.substr(5);
        if (entry != "main" && !g_inView) {
            for (size_t i = 0; i < std::size(kEntries); ++i) {
                const uintptr_t w = Widget(kEntries[i]);
                shown[i] = w && !(*reinterpret_cast<uint16_t*>(w + 0x28) & 0x800);
                if (w && entry != kEntries[i]) setHidden(w, 1);
            }
            g_inView = true;
        } else if (entry == "main" && g_inView) {
            for (size_t i = 0; i < std::size(kEntries); ++i)
                if (const uintptr_t w = Widget(kEntries[i])) setHidden(w, shown[i] ? 0 : 1);
            g_inView = false;
        }
    } else if (ev == "back") {
        PostUiEvent(NetOnServer() ? "rdmp.back.ingame" : "rdmp.back.title");
    } else if (ev == "edit.direct") {
        TextEntryBegin("direct", "Server address (host:port)", g_lastAddress, 63);
    } else if (ev == "edit.nick") {
        TextEntryBegin("nick", "Nickname", g_settings.nickname, 22);
    } else if (ev == "text.ok" || ev == "text.cancel") {
        if (!g_entry.open) return;
        const bool ok = ev == "text.ok";
        std::string text = TextEntryEnd(ok);
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(0, 1);
        if (!ok || text.empty()) return;
        if (g_entry.purpose == "nick") {
            strncpy_s(g_settings.nickname, text.c_str(), _TRUNCATE);
            SaveMpOptions();
        } else if (g_entry.purpose == "direct") {
            // host[:port] (the port defaults to the server's default 27015); joins like the browser (saved under Recent)
            std::string host = text;
            int port = 27015;
            if (const size_t colon = text.rfind(':'); colon != std::string::npos) {
                host = text.substr(0, colon);
                port = atoi(text.c_str() + colon + 1);
            }
            if (host.empty() || port <= 0 || port > 65535) {
                Log("ui: direct connect: \"%s\" is not host:port", text.c_str());
                return;
            }
            g_lastAddress = text;
            Log("ui: direct connect %s:%d", host.c_str(), port);
            NetBrowserJoin(host.c_str(), port);
            JoinStarted();
        }
    } else if (ev == "opt.load") {
        LoadMpOptions();
    } else if (ev == "opt.changed") {
        SaveMpOptions();
    } else if (ev == "disconnect") {
        // Leave the server and stay in this menu, now the main menu: the world stays loaded behind it and the player waits
        // in limbo for the next server (netclient, sandbox ScriptLeaveSession). The pause scene builds its menu column
        // when it opens, from the entries shown then (entries shown later get no row): the Disconnect entry closes it and
        // it opens again with the main menu's entries (rdmp.menu, queued after the entry's actions).
        NetDisconnect();
        g_menuAtMs = GetTickCount64() + 400;  // (posted while the scene is still closing, the push is lost)
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
    for (int i = 0; i < static_cast<int>(std::size(kNametagDistances)); ++i)
        g_texts["RdmpOpt_Distance_" + std::to_string(i)] = std::to_string(kNametagDistances[i]) + " m";
    wcscpy_s(g_dir, iniPath);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_dir), L"ui\\native\\");
    Log("ui files: %s (our menus from %S)", InstallOpenHook() ? "hook installed" : "hook FAILED (bytes mismatch)", g_dir);
}

}  // namespace rdmp
