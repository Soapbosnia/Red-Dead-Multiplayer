#pragma once
/*
 * The main menu's drawing (Dear ImGui, inside the overlay's widget pass): theme, fonts, pages. The data and the
 * connection are frontend.cpp's (menu_model.h).
 */

namespace RDMP {
    class FwClient;
}
namespace rdmp::fw {
    struct Host;
}

namespace RDMP::MenuUi {
    void Attach(FwClient *client, const rdmp::fw::Host *host);
    // The menu came on screen: the main menu (not in a session) or the in-game menu.
    void Opened(bool inSession);
    // One frame of the menu.
    void Draw(bool inSession);
    // Esc: closes a dialog or goes back a page; false when already on the first page.
    bool Back();
} // namespace RDMP::MenuUi
