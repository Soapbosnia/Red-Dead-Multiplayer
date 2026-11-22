#pragma once
/*
 * The in-game overlay: the MafiaHub Framework renderer + ImGui drawn on top of the game's own D3D12 frames.
 *
 * The game owns the device and swapchain; we hook IDXGISwapChain::Present (render thread) and
 * ID3D12CommandQueue::ExecuteCommandLists (to learn which queue presents), hand those to the Framework
 * (InstanceOptions::rendererOptions + Instance::RenderInit, on the game's main thread) and from then on draw
 * the ImGui frame inside every Present. Widgets are pushed from the main thread each frame (Frame()).
 *
 * Input: the game window's procedure is subclassed; ImGui sees every message, and while text is being typed
 * (chat), the cursor is shown or the main menu is up, the keyboard / mouse messages do not reach the game.
 */

#include <functional>

namespace RDMP {
    class FwClient;
}

namespace rdmp::fw {
    struct Host;
}

namespace RDMP::Overlay {
    // Installs the D3D12 / DXGI hooks (main thread, once, after the client exists).
    bool Install(FwClient *client, const rdmp::fw::Host *host);

    // Main thread, once per frame before FwClient::Update(): brings the renderer up once the game's swapchain
    // has been seen, then queues this frame's widget pass (`draw` runs inside the ImGui frame).
    void Frame(const std::function<void()> &draw);

    // True once the renderer and ImGui are running.
    bool Ready();

    // Display size in pixels (0 until Ready()).
    void ScreenSize(float &width, float &height);

    // Mouse cursor for script UIs: shown = the game no longer gets mouse input.
    void ShowCursor(bool show);
    bool CursorShown();

    // The main menu is on screen: it gets the cursor, the mouse and the keyboard; the game gets none of them.
    void SetMenuActive(bool active);
    // Esc never reaches the game (its pause menu does not open); each press is reported once through PollEscape.
    void CaptureEscape(bool capture);
    bool PollEscape();
    // The game window (HWND), null until Ready().
    void *Window();

    // Our UI holds the keyboard: the chat input, an ImGui text field or a focused web view (the main menu).
    bool UiHasKeyboard();

    // A mouse button event for scripts, queued on the window thread and drained on the main thread.
    struct Click {
        int button; // 0 left, 1 right, 2 middle
        bool down;
        float x, y;
    };
    bool PollClick(Click &out);

    void Shutdown();
} // namespace RDMP::Overlay
