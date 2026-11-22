#pragma once
/*
 * The main menu: an HTML page (ui/, next to the binaries) in a full-screen web view of the Framework's CEF GUI,
 * shown whenever this client is not in a server session. Server browser (Internet / Local / Favorites / Recent),
 * direct connect and the Multiplayer settings (nickname, nametags). Game settings (video, controls) stay in the game's
 * own pause menu.
 *
 * Page -> client: callEvent("rdmp:<name>", json) (ui/js/bridge.js); client -> page: rdmp.receive(type, data).
 * Settings live in rdmp.ini ([net] name, [client] ...), favourites and recent servers in servers.json, both next to
 * the binaries.
 */

namespace RDMP {
    class FwClient;
}
namespace rdmp::fw {
    struct Host;
    struct BrowserServer;
    struct Settings;
}

namespace RDMP::Frontend {
    void Attach(FwClient *client, const rdmp::fw::Host *host);
    // Main thread, every frame (after the overlay is up): CEF bring-up, visibility, server query results.
    void Frame();
    // The menu is on screen (it owns mouse and keyboard).
    bool Visible();
    void Shutdown();

    // Server browser for the native menus (fw_api v11); same lists, favourites and recents as the HTML page.
    void BrowserRefresh(const char *list);
    int BrowserPoll(rdmp::fw::BrowserServer *out, int max);
    void BrowserFavorite(const char *host, int port, const char *name, bool add);
    bool BrowserConnect(const char *host, int port, const char *password);

    // Multiplayer settings for the native menus (fw_api v12): the HTML menu's settings, same keys.
    void GetSettings(rdmp::fw::Settings *out);
    void SetSettings(const rdmp::fw::Settings &in);
    // Native menus (fw_api v13): logo and latest changes over the game's main menu while it is on screen.
    void SetMenuOverlay(bool shown);
} // namespace RDMP::Frontend
