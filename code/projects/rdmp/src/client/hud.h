#pragma once
/*
 * What the overlay draws each frame (inside the ImGui pass, main thread): the scripts' dx* drawing for this
 * frame, player nametags and the chat box. Scripts queue their drawing from the clientRender event; the queue is
 * drawn once and cleared, like MTA's dxDraw* in onClientRender.
 */

#include <cstdint>
#include <string>

namespace RDMP {
    class FwClient;
}
namespace rdmp::fw {
    struct Host;
}

namespace RDMP::Hud {
    void Attach(FwClient *client, const rdmp::fw::Host *host);

    // The overlay's per-frame widget pass (Overlay::Frame).
    void Draw();

    // ---- script drawing (this frame) ----
    enum class Align : uint8_t { Left, Center, Right };
    // color: 0xAARRGGBB (tocolor).
    void QueueText(const std::string &text, float x, float y, uint32_t color, float scale, Align align, bool shadow);
    void QueueRect(float x, float y, float w, float h, uint32_t color, float rounding);
    void QueueLine(float x1, float y1, float x2, float y2, uint32_t color, float width);
    void QueueCircle(float x, float y, float radius, uint32_t color, bool filled, float width);
    // Pixel width / height of a text line at `scale` (1 = the UI font's size); 0 before the overlay is up.
    float TextWidth(const std::string &text, float scale);
    float FontHeight(float scale);

    // World (Y up) -> screen pixels; false when behind the camera or no camera.
    bool WorldToScreen(float x, float y, float z, float &sx, float &sy);
    // The camera's position (Y up); false when there is none.
    bool CameraPosition(float &x, float &y, float &z);

    // ---- nametags (local view) ----
    void SetNametagsEnabled(bool enabled); // everyone's
    bool NametagsEnabled();
    // Per player override for this viewer: -1 = follow the server (setPlayerNametagShowing), 0 hide, 1 show.
    void SetNametagOverride(uint64_t playerId, int showing);
    int NametagOverride(uint64_t playerId);
    void SetNametagDrawDistance(float meters);

    // ---- chat ----
    void ChatLine(const std::string &text, uint32_t colorRGBA = 0); // a notice line in this player's chat box
    void SetChatVisible(bool visible);
    bool ChatVisible();
    bool ChatInputActive();
} // namespace RDMP::Hud
