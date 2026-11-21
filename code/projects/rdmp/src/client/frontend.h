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
}

namespace RDMP::Frontend {
    void Attach(FwClient *client, const rdmp::fw::Host *host);
    // Main thread, every frame (after the overlay is up): CEF bring-up, visibility, server query results.
    void Frame();
    // The menu is on screen (it owns mouse and keyboard).
    bool Visible();
    void Shutdown();
} // namespace RDMP::Frontend
