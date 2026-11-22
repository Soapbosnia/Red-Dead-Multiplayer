#pragma once
/*
 * The main menu: Dear ImGui (menu_ui.cpp), drawn by the overlay over the game. MTA-style: the world loads at boot
 * behind it and the menu covers it whenever this client is not in a server session; in a session Esc opens the
 * in-game menu (Resume, Settings, Disconnect, Quit) and the game's own pause menu never opens. While the menu is on
 * screen it owns the mouse and keyboard (the game gets neither, the player does not move).
 *
 * Home (recent servers, changelog), server browser (Internet / Local / Favorites / Recent), direct connect and
 * settings: Multiplayer (nickname, nametags, chat; rdmp.ini [net] / [client]) and the game's own settings (video,
 * graphics, audio, controls; applied through the game, which saves its own files). Favourites and recent servers
 * live in servers.json next to the binaries. [client] menu=0 turns it off (the native title_screen=1 menus).
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
    // Main thread, every frame before the overlay's widget pass: connection state, server query results, Esc.
    void Frame();
    // Inside the overlay's ImGui pass (after the HUD, so on top of it).
    void Draw();
    // The menu is on screen (it owns mouse and keyboard).
    bool Visible();
    // Resume: the in-game menu closes.
    void CloseInGameMenu();
    void Shutdown();

    // Server browser for the native menus (fw_api v11); same lists, favourites and recents as the ImGui menu.
    void BrowserRefresh(const char *list);
    int BrowserPoll(rdmp::fw::BrowserServer *out, int max);
    void BrowserFavorite(const char *host, int port, const char *name, bool add);
    bool BrowserConnect(const char *host, int port, const char *password);

    // Multiplayer settings for the native menus (fw_api v12): the ImGui menu's settings, same keys.
    void GetSettings(rdmp::fw::Settings *out);
    void SetSettings(const rdmp::fw::Settings &in);
} // namespace RDMP::Frontend
