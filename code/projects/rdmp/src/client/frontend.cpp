/*
 * Main menu controller: see frontend.h. The menu's data (menu_model.h) lives here; menu_ui.cpp draws it.
 */

#include <utils/safe_win32.h>

#include <nlohmann/json.hpp>
#include <utils/path.h>

#include "client/frontend.h"
#include "client/fw_client.h"
#include "client/hud.h"
#include "client/menu_model.h"
#include "client/menu_ui.h"
#include "client/overlay.h"
#include "client/script_binds.h"
#include "client/server_query.h"
#include "shared/fw_api.h"
#include "shared/game_options.h"

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace RDMP::Frontend {
    namespace {
        using json  = nlohmann::json;
        using FwPhase = Framework::Integrations::Client::ConnectionPhase;

        FwClient *g_client           = nullptr;
        const rdmp::fw::Host *g_host = nullptr;
        bool g_enabled               = true;  // [client] menu
        bool g_inGameMenu            = false; // the Esc menu over a session
        bool g_visible               = false;

        std::filesystem::path g_binDir;
        std::wstring g_ini;
        json g_servers = {{"favorites", json::array()}, {"recent", json::array()}};
        std::deque<ServerQuery::Info> g_native; // server browser answers for the native menus (BrowserPoll)

        std::map<std::string, std::vector<Model::Server>> g_lists;
        std::map<std::string, uint64_t> g_refreshAt;
        std::map<std::string, std::string> g_listErrors;
        std::vector<std::pair<int, int>> g_resolutions;
        std::vector<int> g_rates;
        Model::Notice g_notice;
        // The game's bindings as it loaded them (before ours from [game_binds]), taken once its table is up.
        std::vector<rdmp::fw::GameBinding> g_gameDefaults;
        bool g_gameBindsApplied = false;

        // What the menu is doing with the connection.
        std::string g_pendingAddress; // "host:port" while connecting / connected from the menu
        bool g_wasConnected = false;
        bool g_leaving      = false; // our own Disconnect (not a drop)

        uint64_t NowMs() {
            return GetTickCount64();
        }

        // ---- settings (rdmp.ini) ----
        std::string IniString(const wchar_t *section, const wchar_t *key, const wchar_t *fallback) {
            wchar_t w[256];
            GetPrivateProfileStringW(section, key, fallback, w, 256, g_ini.c_str());
            char utf8[512];
            WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof(utf8), nullptr, nullptr);
            return utf8;
        }
        int IniInt(const wchar_t *section, const wchar_t *key, int fallback) {
            return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini.c_str()));
        }
        void IniWrite(const wchar_t *section, const wchar_t *key, const std::string &value) {
            wchar_t w[512];
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, w, 512);
            WritePrivateProfileStringW(section, key, w, g_ini.c_str());
        }

        void ApplySettings() {
            Hud::SetNametagsEnabled(IniInt(L"client", L"nametags", 1) != 0);
            Hud::SetNametagDrawDistance(static_cast<float>(IniInt(L"client", L"nametag_distance", 60)));
            Hud::SetChatVisible(IniInt(L"client", L"chat", 1) != 0);
        }

        // ---- servers.json ----
        std::filesystem::path ServersFile() {
            return g_binDir / "servers.json";
        }
        void LoadServers() {
            std::ifstream file(ServersFile());
            if (!file) {
                return;
            }
            const auto doc = json::parse(file, nullptr, false);
            if (doc.is_object()) {
                for (const char *list : {"favorites", "recent"}) {
                    if (doc.contains(list) && doc[list].is_array()) {
                        g_servers[list] = doc[list];
                    }
                }
            }
        }
        void SaveServers() {
            std::ofstream file(ServersFile(), std::ios::trunc);
            file << g_servers.dump(2);
        }
        bool SameServer(const json &entry, const std::string &host, int port) {
            return entry.value("host", std::string()) == host && entry.value("port", 0) == port;
        }
        void Forget(const char *list, const std::string &host, int port) {
            auto &arr = g_servers[list];
            for (auto it = arr.begin(); it != arr.end();) {
                it = SameServer(*it, host, port) ? arr.erase(it) : it + 1;
            }
        }
        void Remember(const char *list, const std::string &host, int port, const std::string &name, size_t keep) {
            Forget(list, host, port);
            auto &arr = g_servers[list];
            arr.insert(arr.begin(), json {{"host", host}, {"port", port}, {"name", name}});
            while (arr.size() > keep) {
                arr.erase(arr.end() - 1);
            }
            SaveServers();
        }

        std::vector<ServerQuery::Address> AddressesOf(const char *list) {
            std::vector<ServerQuery::Address> out;
            for (const auto &e : g_servers[list]) {
                out.push_back({e.value("host", std::string("127.0.0.1")), static_cast<uint16_t>(e.value("port", 27015))});
            }
            return out;
        }

        // A remembered list as rows before the servers answer.
        void SeedRemembered(const char *list) {
            auto &rows = g_lists[list];
            rows.clear();
            for (const auto &e : g_servers[list]) {
                Model::Server row;
                row.info.list = list;
                row.info.host = e.value("host", std::string("127.0.0.1"));
                row.info.port = static_cast<uint16_t>(e.value("port", 27015));
                row.info.name = e.value("name", std::string());
                rows.push_back(row);
            }
        }

        void TakeAnswer(const ServerQuery::Info &info) {
            auto &rows = g_lists[info.list];
            for (auto &row : rows) {
                if (row.info.host == info.host && row.info.port == info.port) {
                    const std::string remembered = row.info.name;
                    row.info                     = info;
                    if (row.info.name.empty()) {
                        row.info.name = remembered;
                    }
                    row.answered = true;
                    return;
                }
            }
            // The browsed lists show only servers that answered; remembered ones are listed either way.
            if (info.online || (info.list != "internet" && info.list != "local")) {
                rows.push_back({info, true});
            }
            // A name the server gave goes into favourites / recents too.
            if (info.online && !info.name.empty()) {
                bool changed = false;
                for (const char *list : {"favorites", "recent"}) {
                    for (auto &e : g_servers[list]) {
                        if (SameServer(e, info.host, info.port) && e.value("name", std::string()) != info.name) {
                            e["name"] = info.name;
                            changed   = true;
                        }
                    }
                }
                if (changed) {
                    SaveServers();
                }
            }
        }

        void LoadDisplayModes() {
            std::set<std::pair<int, int>> sizes;
            std::set<int> rates;
            DEVMODEW mode {};
            mode.dmSize = sizeof(mode);
            for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
                if (mode.dmBitsPerPel >= 24 && mode.dmPelsWidth >= 800) {
                    sizes.insert({static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight)});
                    rates.insert(static_cast<int>(mode.dmDisplayFrequency));
                }
            }
            g_resolutions.assign(sizes.rbegin(), sizes.rend());
            g_rates.assign(rates.begin(), rates.end());
        }

        bool Connected() {
            return g_client && g_client->connected;
        }

        void SetVisible(bool visible) {
            if (visible == g_visible) {
                return;
            }
            g_visible = visible;
            Overlay::SetMenuActive(visible);
            if (visible) {
                MenuUi::Opened(Connected());
            }
        }
    } // namespace

    // =================================================================================================================
    // Model
    // =================================================================================================================
    namespace Model {
        namespace opt = rdmp::opt;

        std::string CleanNickname(std::string name) {
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }), name.end());
            if (name.size() > 22) {
                name.resize(22);
            }
            return name.empty() ? std::string("player") : name;
        }

        MpSettings LoadMp() {
            MpSettings s;
            s.nickname        = CleanNickname(IniString(L"net", L"name", L"player"));
            s.nametags        = IniInt(L"client", L"nametags", 1) != 0;
            s.nametagDistance = IniInt(L"client", L"nametag_distance", 60);
            s.chat            = IniInt(L"client", L"chat", 1) != 0;
            return s;
        }

        void SaveMp(const MpSettings &s) {
            IniWrite(L"net", L"name", CleanNickname(s.nickname));
            IniWrite(L"client", L"nametags", s.nametags ? "1" : "0");
            IniWrite(L"client", L"nametag_distance", std::to_string(std::clamp(s.nametagDistance, 5, 500)));
            IniWrite(L"client", L"chat", s.chat ? "1" : "0");
            ApplySettings();
        }

        bool ReadGame(GameBlocks &out) {
            return g_host && g_host->get_game_options && g_host->get_game_options(out.loaded, out.graphics);
        }

        bool ApplyGame(const GameBlocks &before, const GameBlocks &after, bool &loadedChanged, bool &graphicsChanged) {
            loadedChanged   = std::memcmp(before.loaded, after.loaded, sizeof(after.loaded)) != 0;
            graphicsChanged = std::memcmp(before.graphics, after.graphics, sizeof(after.graphics)) != 0;
            if (!g_host || !g_host->set_game_options) {
                return false;
            }
            if (loadedChanged || graphicsChanged) {
                g_host->set_game_options(loadedChanged ? after.loaded : nullptr, graphicsChanged ? after.graphics : nullptr);
            }
            return true;
        }

        const opt::Field *FindField(const char *key) {
            for (const auto &f : opt::kFields) {
                if (std::strcmp(f.key, key) == 0) {
                    return &f;
                }
            }
            return nullptr;
        }

        double GetField(const opt::Field &f, const GameBlocks &blocks) {
            const uint8_t *p = (f.block == opt::Block::Loaded ? blocks.loaded : blocks.graphics) + f.offset;
            switch (f.type) {
            case opt::Type::Bool:
            case opt::Type::U8: return *p;
            case opt::Type::I32: {
                int32_t v;
                std::memcpy(&v, p, 4);
                return v;
            }
            case opt::Type::F32: {
                float v;
                std::memcpy(&v, p, 4);
                return v;
            }
            }
            return 0.0;
        }

        void SetField(const opt::Field &f, GameBlocks &blocks, double value) {
            uint8_t *p = (f.block == opt::Block::Loaded ? blocks.loaded : blocks.graphics) + f.offset;
            switch (f.type) {
            case opt::Type::Bool: *p = value != 0.0 ? 1 : 0; break;
            case opt::Type::U8: *p = static_cast<uint8_t>(std::clamp(value, 0.0, 255.0)); break;
            case opt::Type::I32: {
                const int32_t v = static_cast<int32_t>(value);
                std::memcpy(p, &v, 4);
            } break;
            case opt::Type::F32: {
                const float v = static_cast<float>(value);
                std::memcpy(p, &v, 4);
            } break;
            }
        }

        const std::vector<std::pair<int, int>> &Resolutions() {
            return g_resolutions;
        }
        const std::vector<int> &RefreshRates() {
            return g_rates;
        }

        const std::vector<Server> &List(const std::string &list) {
            return g_lists[list];
        }

        void Refresh(const std::string &list) {
            g_refreshAt[list] = NowMs();
            g_listErrors.erase(list);
            if (list == "internet") {
                g_lists[list].clear();
                ServerQuery::QueryMasterlist(IniString(L"browser", L"masterlist", L"https://masterlist.mafiahub.dev"),
                                             IniString(L"browser", L"gamemode", L"rdmp"));
            }
            else if (list == "local") {
                // This PC: the default port and the next few (several servers on one machine).
                g_lists[list].clear();
                std::vector<ServerQuery::Address> local;
                for (uint16_t port = 27015; port <= 27020; ++port) {
                    local.push_back({"127.0.0.1", port});
                }
                ServerQuery::QueryAddresses("local", local);
            }
            else if (list == "favorites" || list == "recent") {
                SeedRemembered(list.c_str());
                ServerQuery::QueryAddresses(list, AddressesOf(list.c_str()));
            }
        }

        bool Refreshing(const std::string &list) {
            const auto it = g_refreshAt.find(list);
            if (it == g_refreshAt.end() || NowMs() - it->second > 4000) {
                return false;
            }
            if (list == "favorites" || list == "recent") {
                for (const auto &row : g_lists[list]) {
                    if (!row.answered) {
                        return true;
                    }
                }
                return false;
            }
            return NowMs() - it->second < 2500;
        }

        std::string ListError(const std::string &list) {
            const auto it = g_listErrors.find(list);
            return it == g_listErrors.end() ? std::string() : it->second;
        }

        bool IsFavorite(const std::string &host, int port) {
            for (const auto &e : g_servers["favorites"]) {
                if (SameServer(e, host, port)) {
                    return true;
                }
            }
            return false;
        }

        void SetFavorite(const std::string &host, int port, const std::string &name, bool favorite) {
            if (favorite) {
                Remember("favorites", host, port, name, 200);
                auto &rows = g_lists["favorites"];
                if (std::none_of(rows.begin(), rows.end(), [&](const Server &r) { return r.info.host == host && r.info.port == port; })) {
                    Server row;
                    row.info.list = "favorites";
                    row.info.host = host;
                    row.info.port = static_cast<uint16_t>(port);
                    row.info.name = name;
                    for (const auto &[list, others] : g_lists) {
                        for (const auto &other : others) {
                            if (other.answered && other.info.host == host && other.info.port == port) {
                                row = other;
                            }
                        }
                    }
                    row.info.list = "favorites";
                    rows.insert(rows.begin(), row);
                }
            }
            else {
                Forget("favorites", host, port);
                SaveServers();
                auto &rows = g_lists["favorites"];
                rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Server &r) { return r.info.host == host && r.info.port == port; }), rows.end());
            }
        }

        Phase ConnectionPhase() {
            if (Connected()) {
                return Phase::Connected;
            }
            return g_pendingAddress.empty() ? Phase::Idle : Phase::Connecting;
        }

        std::string PendingAddress() {
            return g_pendingAddress;
        }

        std::string ConnectingStep() {
            switch (g_client ? g_client->GetConnectionPhase() : FwPhase::Disconnected) {
            case FwPhase::Authenticating: return "Signing in";
            case FwPhase::Downloading: return "Downloading the server's resources";
            case FwPhase::Starting: return "Starting the server's scripts";
            case FwPhase::InGame: return "Joining";
            default: return "Connecting";
            }
        }

        bool ParseAddress(const std::string &text, std::string &host, int &port) {
            std::string t = text;
            t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());
            if (t.empty()) {
                return false;
            }
            port             = 27015;
            const size_t pos = t.rfind(':');
            if (pos != std::string::npos) {
                const std::string p = t.substr(pos + 1);
                if (p.empty() || !std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); }) || p.size() > 5) {
                    return false;
                }
                port = std::stoi(p);
                t    = t.substr(0, pos);
            }
            host = t;
            return !host.empty() && port > 0 && port <= 65535;
        }

        void Connect(const std::string &host, int port, const std::string &password) {
            if (host.empty() || port <= 0 || port > 65535 || !g_client) {
                Notify(true, "Enter a server address (host:port).");
                return;
            }
            if (Connected() || !g_pendingAddress.empty()) {
                return;
            }
            const std::string nickname = CleanNickname(IniString(L"net", L"name", L"player"));
            g_pendingAddress           = host + ":" + std::to_string(port);
            if (!g_client->Connect(host, port, nickname, password)) {
                Notify(true, "Could not start a connection to " + g_pendingAddress + ".");
                g_pendingAddress.clear();
                return;
            }
            std::string name;
            for (const auto &[list, rows] : g_lists) {
                for (const auto &row : rows) {
                    if (row.info.host == host && row.info.port == port && !row.info.name.empty()) {
                        name = row.info.name;
                    }
                }
            }
            Remember("recent", host, port, name, 20);
        }

        void Disconnect() {
            auto *net = g_client ? g_client->GetNetworkingEngine() : nullptr;
            if (auto *peer = net ? net->GetNetworkClient() : nullptr) {
                g_leaving = Connected();
                (void)peer->Disconnect();
            }
            g_pendingAddress.clear();
            g_inGameMenu = false;
        }

        void Quit() {
            // The game ends the way its own exit does (rdmp.dll stops our game loop); a WM_CLOSE only made it beep.
            if (g_host && g_host->quit) {
                g_host->quit();
            }
        }

        int PingMs() {
            return g_client ? g_client->PingMs() : -1;
        }

        std::string Version() {
            return g_client ? g_client->GetOptions().modVersion : std::string();
        }

        const Notice &LastNotice() {
            return g_notice;
        }

        void Notify(bool error, const std::string &text) {
            g_notice = {error, text, NowMs()};
        }

        std::vector<rdmp::fw::GameBinding> GameBindings() {
            std::vector<rdmp::fw::GameBinding> out(256);
            const int n = g_host && g_host->get_bindings ? g_host->get_bindings(out.data(), static_cast<int>(out.size())) : 0;
            out.resize(static_cast<size_t>(std::max(0, n)));
            return out;
        }

        bool SetGameBinding(const std::string &action, int slot, int type, int input) {
            if (!g_host || !g_host->set_binding || g_host->set_binding(action.c_str(), slot, type, input) == 0) {
                return false;
            }
            // Kept in our own file, the whole action: "type input type input" (primary, secondary).
            for (const auto &b : GameBindings()) {
                if (action == b.action) {
                    char value[64];
                    std::snprintf(value, sizeof(value), "%d %d %d %d", b.type[0], b.input[0], b.type[1], b.input[1]);
                    IniWrite(L"game_binds", std::wstring(action.begin(), action.end()).c_str(), value);
                }
            }
            return true;
        }

        bool GameBindingDefault(const std::string &action, rdmp::fw::GameBinding &out) {
            for (const auto &b : g_gameDefaults) {
                if (action == b.action) {
                    out = b;
                    return true;
                }
            }
            return false;
        }

        void ResetGameBindings() {
            const auto now = GameBindings();
            for (const auto &d : g_gameDefaults) {
                for (const auto &b : now) {
                    if (std::strcmp(b.action, d.action) != 0) {
                        continue;
                    }
                    for (int slot = 0; slot < 2; ++slot) {
                        if (b.type[slot] != d.type[slot] || b.input[slot] != d.input[slot]) {
                            g_host->set_binding(d.action, slot, d.type[slot], d.input[slot]);
                        }
                    }
                }
            }
            WritePrivateProfileStringW(L"game_binds", nullptr, nullptr, g_ini.c_str());
        }

        std::string ChatKey() {
            const std::string key = IniString(L"binds", L"chat", L"t");
            return ScriptBinds::KeyCode(key) ? key : std::string("t");
        }

        void SetChatKey(const std::string &key) {
            IniWrite(L"binds", L"chat", key);
            Overlay::SetChatKey(ScriptBinds::KeyCode(ChatKey()));
        }
    } // namespace Model

    // =================================================================================================================
    // Flow
    // =================================================================================================================
    void Attach(FwClient *client, const rdmp::fw::Host *host) {
        g_client  = client;
        g_host    = host;
        g_binDir  = std::filesystem::path(Framework::Utils::GetModuleDirW());
        g_ini     = (g_binDir / "rdmp.ini").wstring();
        g_enabled = IniInt(L"client", L"menu", 1) != 0;
        MenuUi::Attach(client, host);
        LoadServers();
        LoadDisplayModes();
        ApplySettings();
        SeedRemembered("favorites");
        SeedRemembered("recent");
        // The game's own pause menu never opens: Esc is ours (the in-game menu).
        Overlay::CaptureEscape(g_enabled);
        Overlay::SetChatKey(ScriptBinds::KeyCode(Model::ChatKey()));
    }

    void Frame() {
        if (!g_client) {
            return;
        }
        const bool connected = Connected();
        if (connected && !g_wasConnected) {
            g_inGameMenu = false;
            g_leaving    = false;
        }
        if (!connected && g_wasConnected) {
            g_pendingAddress.clear();
            if (!g_leaving) {
                Model::Notify(true, "Disconnected from the server.");
            }
            g_leaving    = false;
            g_inGameMenu = false;
        }
        if (!connected && !g_pendingAddress.empty() && g_client->GetConnectionPhase() == FwPhase::Disconnected) {
            Model::Notify(true, "Could not connect to " + g_pendingAddress + ".");
            g_pendingAddress.clear();
        }
        g_wasConnected = connected;

        // Our game binds over the game's own, once its table is up (MTA-style: the mod's config is applied at launch).
        if (!g_gameBindsApplied && g_host && g_host->get_bindings && g_host->set_binding) {
            const auto loaded = Model::GameBindings();
            if (!loaded.empty()) {
                g_gameBindsApplied = true;
                g_gameDefaults     = loaded;
                std::vector<wchar_t> section(32768);
                const DWORD len = GetPrivateProfileSectionW(L"game_binds", section.data(), static_cast<DWORD>(section.size()), g_ini.c_str());
                int applied     = 0;
                for (const wchar_t *p = section.data(); p < section.data() + len && *p; p += std::wcslen(p) + 1) {
                    const std::wstring line(p);
                    const size_t eq = line.find(L'=');
                    if (eq == std::wstring::npos) {
                        continue;
                    }
                    std::string action; // action names are ASCII
                    for (size_t k = 0; k < eq; ++k) {
                        action.push_back(static_cast<char>(line[k]));
                    }
                    int t[2] = {0, 0}, in[2] = {-1, -1};
                    if (swscanf_s(line.c_str() + eq + 1, L"%d %d %d %d", &t[0], &in[0], &t[1], &in[1]) != 4) {
                        continue;
                    }
                    for (const auto &b : loaded) {
                        if (action != b.action) {
                            continue;
                        }
                        for (int slot = 0; slot < 2; ++slot) {
                            if (b.type[slot] != t[slot] || b.input[slot] != in[slot]) {
                                applied += g_host->set_binding(b.action, slot, t[slot], in[slot]);
                            }
                        }
                    }
                }
                Log()->info("menu: {} game binding(s) applied from rdmp.ini [game_binds]", applied);
            }
        }

        for (const auto &info : ServerQuery::Poll()) {
            if (g_native.size() < 1024) {
                g_native.push_back(info);
            }
            TakeAnswer(info);
        }
        std::string list, message;
        while (ServerQuery::PollError(list, message)) {
            g_listErrors[list] = message;
        }

        if (!g_enabled) {
            return;
        }
        if (Overlay::PollEscape() && Overlay::Ready()) {
            if (g_visible) {
                if (!MenuUi::Back() && connected) {
                    g_inGameMenu = false; // Esc on the in-game menu's first page: back to the game
                }
            }
            else if (connected) {
                g_inGameMenu = true;
            }
        }
        SetVisible(Overlay::Ready() && (!connected || g_inGameMenu || g_leaving)); // leaving: until the session is closed
    }

    void Draw() {
        if (g_enabled && g_visible) {
            MenuUi::Draw(Connected());
        }
    }

    void CloseInGameMenu() {
        g_inGameMenu = false;
    }

    bool Visible() {
        return g_visible;
    }

    void Shutdown() {
        ServerQuery::Shutdown();
    }

    // ---- native menus (fw_api v11 / v12) ----
    void BrowserRefresh(const char *list) {
        if (list) {
            Model::Refresh(list);
        }
    }

    int BrowserPoll(rdmp::fw::BrowserServer *out, int max) {
        int n = 0;
        while (n < max && !g_native.empty()) {
            const ServerQuery::Info &info = g_native.front();
            rdmp::fw::BrowserServer &o    = out[n++];
            o                             = {};
            strncpy_s(o.list, info.list.c_str(), _TRUNCATE);
            strncpy_s(o.host, info.host.c_str(), _TRUNCATE);
            strncpy_s(o.name, info.name.c_str(), _TRUNCATE);
            o.port       = info.port;
            o.online     = info.online ? 1 : 0;
            o.players    = info.players;
            o.maxPlayers = info.maxPlayers;
            o.password   = info.password ? 1 : 0;
            o.pingMs     = info.pingMs;
            o.favorite   = Model::IsFavorite(info.host, info.port) ? 1 : 0;
            g_native.pop_front();
        }
        return n;
    }

    void BrowserFavorite(const char *host, int port, const char *name, bool add) {
        if (host) {
            Model::SetFavorite(host, port, name ? name : "", add);
        }
    }

    bool BrowserConnect(const char *host, int port, const char *password) {
        if (!host || !g_client) {
            return false;
        }
        Model::Connect(host, port, password ? password : "");
        return !g_pendingAddress.empty();
    }

    void GetSettings(rdmp::fw::Settings *out) {
        if (!out) {
            return;
        }
        *out                 = {};
        const auto s         = Model::LoadMp();
        strncpy_s(out->nickname, s.nickname.c_str(), _TRUNCATE);
        out->nametags        = s.nametags ? 1 : 0;
        out->nametagDistance = s.nametagDistance;
        out->chat            = s.chat ? 1 : 0;
    }

    void SetSettings(const rdmp::fw::Settings &in) {
        Model::MpSettings s;
        s.nickname        = std::string(in.nickname, strnlen(in.nickname, sizeof(in.nickname)));
        s.nametags        = in.nametags != 0;
        s.nametagDistance = in.nametagDistance;
        s.chat            = in.chat != 0;
        Model::SaveMp(s);
    }
} // namespace RDMP::Frontend
