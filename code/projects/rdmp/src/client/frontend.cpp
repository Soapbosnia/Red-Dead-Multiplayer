/*
 * Main menu controller: see frontend.h.
 */

#include <utils/safe_win32.h>

#include <gui/manager.h>
#include <gui/view.h>
#include <nlohmann/json.hpp>
#include <utils/path.h>

#include "client/frontend.h"
#include "client/fw_client.h"
#include "client/hud.h"
#include "client/overlay.h"
#include "client/server_query.h"
#include "shared/fw_api.h"
#include "shared/game_options.h"

#include <algorithm>
#include <deque>
#include <cstring>
#include <set>
#include <filesystem>
#include <fstream>
#include <imgui.h>

namespace RDMP::Frontend {
    namespace {
        using json = nlohmann::json;

        FwClient *g_client           = nullptr;
        const rdmp::fw::Host *g_host = nullptr;
        bool g_failed                = false;
        int g_viewId                 = 0;
        bool g_pageReady             = false;
        bool g_visible               = false;
        float g_viewW = 0, g_viewH = 0;

        std::filesystem::path g_binDir;
        std::wstring g_ini;
        json g_servers = {{"favorites", json::array()}, {"recent", json::array()}};
        std::deque<ServerQuery::Info> g_native; // server browser answers for the native menus (BrowserPoll)

        // What the menu is doing with the connection.
        std::string g_pendingAddress; // "host:port" while connecting from the menu
        bool g_wasConnected = false;

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

        json Settings() {
            return {{"nickname", IniString(L"net", L"name", L"player")},
                    {"nametags", IniInt(L"client", L"nametags", 1) != 0},
                    {"nametagDistance", IniInt(L"client", L"nametag_distance", 60)},
                    {"chat", IniInt(L"client", L"chat", 1) != 0}};
        }

        void ApplySettings() {
            Hud::SetNametagsEnabled(IniInt(L"client", L"nametags", 1) != 0);
            Hud::SetNametagDrawDistance(static_cast<float>(IniInt(L"client", L"nametag_distance", 60)));
            Hud::SetChatVisible(IniInt(L"client", L"chat", 1) != 0);
        }

        std::string CleanNickname(std::string name) {
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }), name.end());
            if (name.size() > 22) {
                name.resize(22);
            }
            return name.empty() ? std::string("player") : name;
        }

        // ---- the game's own settings (shared/game_options.h, through rdmp.dll) ----
        namespace opt = rdmp::opt;
        void Send(const std::string &type, const json &data); // page bridge, below

        uint8_t *BlockOf(const opt::Field &f, uint8_t *loaded, uint8_t *graphics) {
            return f.block == opt::Block::Loaded ? loaded : graphics;
        }

        json ReadField(const opt::Field &f, const uint8_t *base) {
            const uint8_t *p = base + f.offset;
            switch (f.type) {
            case opt::Type::Bool: return *p != 0;
            case opt::Type::U8: return static_cast<int>(*p);
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
            return nullptr;
        }

        bool WriteField(const opt::Field &f, uint8_t *base, const json &value) {
            uint8_t *p = base + f.offset;
            uint8_t before[4];
            std::memcpy(before, p, 4);
            switch (f.type) {
            case opt::Type::Bool: *p = value.is_boolean() ? value.get<bool>() : value.is_number() && value.get<double>() != 0.0; break;
            case opt::Type::U8: if (value.is_number()) *p = static_cast<uint8_t>(std::clamp(value.get<double>(), 0.0, 255.0)); break;
            case opt::Type::I32: if (value.is_number()) { const int32_t v = static_cast<int32_t>(value.get<double>()); std::memcpy(p, &v, 4); } break;
            case opt::Type::F32: if (value.is_number()) { const float v = static_cast<float>(value.get<double>()); std::memcpy(p, &v, 4); } break;
            }
            return std::memcmp(before, p, f.type == opt::Type::Bool || f.type == opt::Type::U8 ? 1 : 4) != 0;
        }

        // The monitor's display modes: resolutions (largest first) and the refresh rates each offers.
        json DisplayModes() {
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
            json res = json::array(), hz = json::array();
            for (auto it = sizes.rbegin(); it != sizes.rend(); ++it) {
                res.push_back({it->first, it->second});
            }
            for (int r : rates) {
                hz.push_back(r);
            }
            return {{"resolutions", res}, {"refreshRates", hz}};
        }

        json GameSettings() {
            uint8_t loaded[opt::kLoadedSize] {}, graphics[opt::kGraphicsSize] {};
            if (!g_host || !g_host->get_game_options || !g_host->get_game_options(loaded, graphics)) {
                return {{"available", false}};
            }
            json values = json::object();
            for (const auto &f : opt::kFields) {
                values[f.key] = ReadField(f, BlockOf(f, loaded, graphics));
            }
            return {{"available", true}, {"values", values}, {"modes", DisplayModes()}};
        }

        // Applies the page's values: only the blocks that changed go to the game (graphics re-applies the renderer).
        void ApplyGameSettings(const json &values) {
            uint8_t loaded[opt::kLoadedSize] {}, graphics[opt::kGraphicsSize] {};
            if (!g_host || !g_host->set_game_options || !g_host->get_game_options(loaded, graphics) || !values.is_object()) {
                Send("status", {{"state", "error"}, {"message", "The game's settings are not available right now."}});
                return;
            }
            bool loadedChanged = false, graphicsChanged = false;
            for (const auto &f : opt::kFields) {
                if (values.contains(f.key) && WriteField(f, BlockOf(f, loaded, graphics), values[f.key])) {
                    (f.block == opt::Block::Loaded ? loadedChanged : graphicsChanged) = true;
                }
            }
            if (loadedChanged || graphicsChanged) {
                g_host->set_game_options(loadedChanged ? loaded : nullptr, graphicsChanged ? graphics : nullptr);
            }
            Send("gameSettings", GameSettings());
            Send("status", {{"state", "ok"}, {"message", loadedChanged || graphicsChanged ? "Game settings applied." : "Nothing changed."}});
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
        void Remember(const char *list, const std::string &host, int port, const std::string &name, size_t keep) {
            auto &arr = g_servers[list];
            for (auto it = arr.begin(); it != arr.end();) {
                it = SameServer(*it, host, port) ? arr.erase(it) : it + 1;
            }
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

        // ---- page bridge ----
        Framework::GUI::View *View() {
            auto *web = g_client ? g_client->GetWebManager() : nullptr;
            return web && g_viewId ? web->GetView(g_viewId) : nullptr;
        }

        void Send(const std::string &type, const json &data) {
            auto *view = View();
            if (!view || !g_pageReady) {
                return;
            }
            const json message = {{"type", type}, {"data", data}};
            view->EvaluateScript("window.rdmp && window.rdmp.receive(" + message.dump() + ");");
        }

        void Refresh(const std::string &list) {
            if (list == "internet") {
                ServerQuery::QueryMasterlist(IniString(L"browser", L"masterlist", L"https://masterlist.mafiahub.dev"),
                                             IniString(L"browser", L"gamemode", L"rdmp"));
            }
            else if (list == "local") {
                // This PC: the default port and the next few (several servers on one machine).
                std::vector<ServerQuery::Address> local;
                for (uint16_t port = 27015; port <= 27020; ++port) {
                    local.push_back({"127.0.0.1", port});
                }
                ServerQuery::QueryAddresses("local", local);
            }
            else if (list == "favorites" || list == "recent") {
                ServerQuery::QueryAddresses(list, AddressesOf(list.c_str()));
            }
        }

        void Connect(const std::string &host, int port, const std::string &password) {
            if (host.empty() || port <= 0 || port > 65535 || !g_client) {
                Send("status", {{"state", "error"}, {"message", "Enter a server address (host:port)."}});
                return;
            }
            const std::string nickname = CleanNickname(IniString(L"net", L"name", L"player"));
            g_pendingAddress           = host + ":" + std::to_string(port);
            Send("status", {{"state", "connecting"}, {"message", "Connecting to " + g_pendingAddress + " ..."}});
            if (!g_client->Connect(host, port, nickname, password)) {
                Send("status", {{"state", "error"}, {"message", "Could not start a connection to " + g_pendingAddress + "."}});
                g_pendingAddress.clear();
                return;
            }
            Remember("recent", host, port, "", 20);
            Send("lists", g_servers);
        }

        void OnPageEvent(const std::string &name, const std::string &payload) {
            const json data = json::parse(payload.empty() ? "{}" : payload, nullptr, false);
            if (name == "ready") {
                g_pageReady = true;
                Send("settings", Settings());
                Send("lists", g_servers);
                Send("version", {{"version", g_client ? g_client->GetOptions().modVersion : ""}});
                const json game = GameSettings();
                Send("gameSettings", game);
                if (game.value("available", false)) {
                    const auto &v = game["values"];
                    Log()->info("menu: game settings read: brightness {} mouse {} sfx {} music {} | {}x{} @ {} Hz, streaming {} m",
                                v["display.fBrightness"].dump(), v["kbMouse.fMouseScalarSensitivity"].dump(), v["audio.fSFXVolume"].dump(),
                                v["audio.fMusicVolume"].dump(), v["ResolutionX"].dump(), v["ResolutionY"].dump(), v["RefreshRate"].dump(),
                                v["WorldStreamingRadius"].dump());
                }
            }
            else if (name == "refresh" && data.is_object()) {
                Refresh(data.value("list", std::string("local")));
            }
            else if (name == "connect" && data.is_object()) {
                Connect(data.value("host", std::string()), data.value("port", 27015), data.value("password", std::string()));
            }
            else if (name == "favorite" && data.is_object()) {
                const std::string host = data.value("host", std::string());
                const int port         = data.value("port", 27015);
                if (data.value("add", true)) {
                    Remember("favorites", host, port, data.value("name", std::string()), 200);
                }
                else {
                    auto &arr = g_servers["favorites"];
                    for (auto it = arr.begin(); it != arr.end();) {
                        it = SameServer(*it, host, port) ? arr.erase(it) : it + 1;
                    }
                    SaveServers();
                }
                Send("lists", g_servers);
            }
            else if (name == "settings" && data.is_object()) {
                IniWrite(L"net", L"name", CleanNickname(data.value("nickname", std::string("player"))));
                IniWrite(L"client", L"nametags", data.value("nametags", true) ? "1" : "0");
                IniWrite(L"client", L"nametag_distance", std::to_string(std::clamp(data.value("nametagDistance", 60), 5, 500)));
                IniWrite(L"client", L"chat", data.value("chat", true) ? "1" : "0");
                ApplySettings();
                Send("settings", Settings());
                Send("status", {{"state", "info"}, {"message", "Settings saved."}});
            }
            else if (name == "gameSettings" && data.is_object()) {
                ApplyGameSettings(data.value("values", json::object()));
            }
            else if (name == "quit") {
                if (HWND window = GetForegroundWindow()) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            }
        }

        void BringUp() {
            auto *web = g_client->GetWebManager();
            if (!web || !g_client->GetRenderer()) {
                return;
            }
            float w, h;
            Overlay::ScreenSize(w, h);
            if (w <= 0 || h <= 0) {
                return; // the first widget pass has not run yet
            }
            if (auto res = web->Init(g_binDir.string(), {static_cast<int>(w), static_cast<int>(h)}, g_client->GetRenderer(), false); !res) {
                g_failed = true;
                Log()->error("menu: the web UI did not start: {}", res.GetError().message);
                return;
            }
            web->RegisterResourceDirectory("rdmp", g_binDir / "ui");
            g_viewId = web->CreateView(Framework::GUI::Manager::ResourceURL("rdmp", "index.html"), 0, 0);
            auto *view = web->GetView(g_viewId);
            if (!view) {
                g_failed = true;
                Log()->error("menu: could not create the menu view");
                return;
            }
            view->SetZIndex(1000);
            view->SetAlwaysComposite(true);
            for (const char *event : {"ready", "refresh", "connect", "favorite", "settings", "gameSettings", "quit"}) {
                const std::string name = event;
                view->AddEventListener("rdmp:" + name, [name](const std::string &payload) {
                    OnPageEvent(name, payload);
                });
            }
            g_viewW = w, g_viewH = h;
            Log()->info("menu: web UI up ({}x{})", w, h);
        }

        void SetVisible(bool visible) {
            auto *view = View();
            if (!view || visible == g_visible) {
                return;
            }
            g_visible = visible;
            view->Display(visible);
            view->Focus(visible);
            Overlay::ShowCursor(visible);
        }
    } // namespace

    void Attach(FwClient *client, const rdmp::fw::Host *host) {
        g_client = client;
        g_host   = host;
        g_binDir = std::filesystem::path(Framework::Utils::GetModuleDirW());
        g_ini    = (g_binDir / "rdmp.ini").wstring();
        LoadServers();
        ApplySettings();
    }

    void Frame() {
        if (!g_client || g_failed) {
            return;
        }
        // [client] html_menu=0: the game's own front end shows our menus (native); no web view, the server browser
        // answers go to BrowserPoll only.
        const bool html = IniInt(L"client", L"html_menu", 1) != 0;
        if (html) {
            if (!Overlay::Ready()) {
                return;
            }
            if (!g_viewId) {
                BringUp();
                return;
            }
            // Track the window size.
            float w, h;
            Overlay::ScreenSize(w, h);
            if (w > 0 && h > 0 && (w != g_viewW || h != g_viewH)) {
                g_viewW = w, g_viewH = h;
                g_client->GetWebManager()->Resize(static_cast<int>(w), static_cast<int>(h));
            }
        }

        // Shown while not in a session.
        const bool connected = g_client->connected;
        if (connected && !g_wasConnected) {
            Send("status", {{"state", "connected"}, {"message", "Connected."}});
            g_pendingAddress.clear();
        }
        if (!connected && g_wasConnected) {
            Send("status", {{"state", "error"}, {"message", "Disconnected from the server."}});
        }
        if (!connected && !g_pendingAddress.empty() &&
            g_client->GetConnectionPhase() == Framework::Integrations::Client::ConnectionPhase::Disconnected) {
            Send("status", {{"state", "error"}, {"message", "Could not connect to " + g_pendingAddress + "."}});
            g_pendingAddress.clear();
        }
        g_wasConnected = connected;
        if (html) {
            SetVisible(!connected);
        }

        for (const auto &info : ServerQuery::Poll()) {
            if (g_native.size() < 1024) {
                g_native.push_back(info);
            }
            Send("server", {{"list", info.list}, {"host", info.host}, {"port", info.port}, {"online", info.online}, {"name", info.name},
                            {"version", info.version}, {"players", info.players}, {"maxPlayers", info.maxPlayers},
                            {"password", info.password}, {"ping", info.pingMs}});
        }
        std::string list, message;
        while (ServerQuery::PollError(list, message)) {
            Send("listError", {{"list", list}, {"message", message}});
        }
    }

    bool Visible() {
        return g_visible;
    }

    void Shutdown() {
        ServerQuery::Shutdown();
    }

    void BrowserRefresh(const char *list) {
        if (list) {
            Refresh(list);
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
            for (const auto &e : g_servers["favorites"]) {
                o.favorite |= SameServer(e, info.host, info.port) ? 1 : 0;
            }
            g_native.pop_front();
        }
        return n;
    }

    void BrowserFavorite(const char *host, int port, const char *name, bool add) {
        if (!host) {
            return;
        }
        if (add) {
            Remember("favorites", host, port, name ? name : "", 200);
        }
        else {
            auto &arr = g_servers["favorites"];
            for (auto it = arr.begin(); it != arr.end();) {
                it = SameServer(*it, host, port) ? arr.erase(it) : it + 1;
            }
            SaveServers();
        }
    }

    bool BrowserConnect(const char *host, int port, const char *password) {
        if (!host || !g_client) {
            return false;
        }
        Connect(host, port, password ? password : "");
        return !g_pendingAddress.empty();
    }

    void GetSettings(rdmp::fw::Settings *out) {
        if (!out) {
            return;
        }
        *out = {};
        strncpy_s(out->nickname, CleanNickname(IniString(L"net", L"name", L"player")).c_str(), _TRUNCATE);
        out->nametags        = IniInt(L"client", L"nametags", 1) != 0 ? 1 : 0;
        out->nametagDistance = IniInt(L"client", L"nametag_distance", 60);
        out->chat            = IniInt(L"client", L"chat", 1) != 0 ? 1 : 0;
    }

    void SetSettings(const rdmp::fw::Settings &in) {
        std::string nickname(in.nickname, strnlen(in.nickname, sizeof(in.nickname)));
        IniWrite(L"net", L"name", CleanNickname(nickname));
        IniWrite(L"client", L"nametags", in.nametags ? "1" : "0");
        IniWrite(L"client", L"nametag_distance", std::to_string(std::clamp(static_cast<int>(in.nametagDistance), 5, 500)));
        IniWrite(L"client", L"chat", in.chat ? "1" : "0");
        ApplySettings();
    }
} // namespace RDMP::Frontend
