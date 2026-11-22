#pragma once
/*
 * What the ImGui main menu (menu_ui.cpp) shows and does, kept by frontend.cpp: the multiplayer settings (rdmp.ini),
 * the game's own settings (shared/game_options.h, through rdmp.dll), the server lists (servers.json + ServerQuery),
 * the connection and the changelog. Main thread only.
 */

#include "client/server_query.h"
#include "shared/game_options.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RDMP::Frontend::Model {
    // ---- multiplayer settings ([net] name, [client] nametags / nametag_distance / chat) ----
    struct MpSettings {
        std::string nickname;
        bool nametags       = true;
        int nametagDistance = 60;
        bool chat           = true;
        bool operator==(const MpSettings &) const = default;
    };
    MpSettings LoadMp();
    void SaveMp(const MpSettings &settings);
    std::string CleanNickname(std::string name);

    // ---- the game's settings: the raw LoadedOptions / GraphicsOptions blocks ----
    struct GameBlocks {
        uint8_t loaded[rdmp::opt::kLoadedSize] {};
        uint8_t graphics[rdmp::opt::kGraphicsSize] {};
    };
    bool ReadGame(GameBlocks &out);
    // Sends the blocks that differ from `before` to the game (graphics re-applies the renderer).
    bool ApplyGame(const GameBlocks &before, const GameBlocks &after, bool &loadedChanged, bool &graphicsChanged);
    const rdmp::opt::Field *FindField(const char *key);
    double GetField(const rdmp::opt::Field &f, const GameBlocks &blocks);
    void SetField(const rdmp::opt::Field &f, GameBlocks &blocks, double value);
    // The monitor's modes: resolutions (largest first) and refresh rates.
    const std::vector<std::pair<int, int>> &Resolutions();
    const std::vector<int> &RefreshRates();

    // ---- servers ----
    struct Server {
        ServerQuery::Info info;
        bool answered = false; // false: still being asked (or only remembered)
    };
    // "internet", "local", "favorites", "recent".
    const std::vector<Server> &List(const std::string &list);
    void Refresh(const std::string &list);
    bool Refreshing(const std::string &list); // a request for it is out
    std::string ListError(const std::string &list);
    bool IsFavorite(const std::string &host, int port);
    void SetFavorite(const std::string &host, int port, const std::string &name, bool favorite);

    // ---- connection ----
    enum class Phase { Idle, Connecting, Connected };
    Phase ConnectionPhase();
    std::string PendingAddress();   // "host:port" being connected to / connected to
    std::string ConnectingStep();   // "Connecting", "Downloading resources", ... while connecting
    void Connect(const std::string &host, int port, const std::string &password);
    // "host[:port]" -> host, port (27015 when left out); false when it cannot be read.
    bool ParseAddress(const std::string &text, std::string &host, int &port);
    void Disconnect();
    void Quit();
    int PingMs();
    std::string Version();

    // ---- messages for a toast ----
    struct Notice {
        bool error = false;
        std::string text;
        uint64_t atMs = 0;
    };
    const Notice &LastNotice();
    void Notify(bool error, const std::string &text);

    // ---- ui/changelog.txt: "# heading" starts a version, "- " a line ----
    struct ChangelogEntry {
        std::string heading;
        std::vector<std::string> lines;
    };
    const std::vector<ChangelogEntry> &Changelog();
} // namespace RDMP::Frontend::Model
