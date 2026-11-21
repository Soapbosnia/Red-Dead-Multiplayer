/*
 * CLIENT drawing, screen and HUD functions:
 *   dxDrawText(text, x, y[, color, scale, alignX = "left", shadow = false])   this frame only (call from clientRender)
 *   dxDrawRectangle(x, y, width, height[, color, rounding])
 *   dxDrawLine(x1, y1, x2, y2[, color, width])
 *   dxDrawCircle(x, y, radius[, color, filled = true, width])
 *   dxGetTextWidth(text[, scale]) / dxGetFontHeight([scale])   pixels
 *   guiGetScreenSize() -> {x, y}
 *   getScreenFromWorldPosition(x, y, z) -> {x, y} | false
 *   getCameraPosition() -> {x, y, z} | false
 *   showCursor(show) / isCursorShowing() / getCursorPosition() -> {x, y} | false
 *   outputChatBox(text[, r, g, b])            a line in this player's chat box
 *   showChat(visible) / isChatVisible() / isChatInputActive()
 *   showGameText(text[, ms = 4000, style = "subtitle"])   the game's HUD text ("subtitle" | "big")
 *   setPlayerNametagShowing(player, showing | null)   this viewer's override (null = follow the server)
 *   showPlayerNametags(enabled) / setNametagDrawDistance(meters)
 * Colours are tocolor(r, g, b[, a]) numbers (0xAARRGGBB).
 */

#include <utils/safe_win32.h>

#include "client/api.h"
#include "client/fw_client.h"
#include "client/hud.h"
#include "client/overlay.h"

namespace RDMP::ClientApi {
    namespace {
        using namespace ScriptApi;

        Hud::Align AlignArg(const Args &args, int i) {
            const std::string a = Str(args, i, "left");
            return a == "center" ? Hud::Align::Center : a == "right" ? Hud::Align::Right : Hud::Align::Left;
        }

        void DxDrawTextFn(const Args &args) {
            if (args.Length() < 3) {
                Throw(args.GetIsolate(), "dxDrawText(text, x, y[, color, scale, alignX, shadow])");
                return;
            }
            Hud::QueueText(Str(args, 0), Float(args, 1, 0), Float(args, 2, 0), Color(args, 3, 0xFFFFFFFF), Float(args, 4, 1.0f), AlignArg(args, 5),
                           Bool(args, 6, false));
            args.GetReturnValue().Set(true);
        }
        void DxDrawRectangleFn(const Args &args) {
            if (args.Length() < 4) {
                Throw(args.GetIsolate(), "dxDrawRectangle(x, y, width, height[, color, rounding])");
                return;
            }
            Hud::QueueRect(Float(args, 0, 0), Float(args, 1, 0), Float(args, 2, 0), Float(args, 3, 0), Color(args, 4, 0xFFFFFFFF), Float(args, 5, 0));
            args.GetReturnValue().Set(true);
        }
        void DxDrawLineFn(const Args &args) {
            if (args.Length() < 4) {
                Throw(args.GetIsolate(), "dxDrawLine(x1, y1, x2, y2[, color, width])");
                return;
            }
            Hud::QueueLine(Float(args, 0, 0), Float(args, 1, 0), Float(args, 2, 0), Float(args, 3, 0), Color(args, 4, 0xFFFFFFFF), Float(args, 5, 1));
            args.GetReturnValue().Set(true);
        }
        void DxDrawCircleFn(const Args &args) {
            if (args.Length() < 3) {
                Throw(args.GetIsolate(), "dxDrawCircle(x, y, radius[, color, filled, width])");
                return;
            }
            Hud::QueueCircle(Float(args, 0, 0), Float(args, 1, 0), Float(args, 2, 0), Color(args, 3, 0xFFFFFFFF), Bool(args, 4, true), Float(args, 5, 1));
            args.GetReturnValue().Set(true);
        }
        void DxGetTextWidthFn(const Args &args) {
            args.GetReturnValue().Set(static_cast<double>(Hud::TextWidth(Str(args, 0), Float(args, 1, 1.0f))));
        }
        void DxGetFontHeightFn(const Args &args) {
            args.GetReturnValue().Set(static_cast<double>(Hud::FontHeight(Float(args, 0, 1.0f))));
        }

        void GuiGetScreenSizeFn(const Args &args) {
            float w, h;
            Overlay::ScreenSize(w, h);
            args.GetReturnValue().Set(Vec2(args.GetIsolate(), w, h));
        }
        void GetScreenFromWorldPositionFn(const Args &args) {
            float sx, sy;
            if (args.Length() < 3 || !Hud::WorldToScreen(Float(args, 0, 0), Float(args, 1, 0), Float(args, 2, 0), sx, sy)) {
                args.GetReturnValue().Set(false);
                return;
            }
            args.GetReturnValue().Set(Vec2(args.GetIsolate(), sx, sy));
        }
        void GetCameraPositionFn(const Args &args) {
            float x, y, z;
            if (!Hud::CameraPosition(x, y, z)) {
                args.GetReturnValue().Set(false);
                return;
            }
            args.GetReturnValue().Set(Vec3(args.GetIsolate(), x, y, z));
        }

        void ShowCursorFn(const Args &args) {
            Overlay::ShowCursor(Bool(args, 0, true));
            args.GetReturnValue().Set(true);
        }
        void IsCursorShowingFn(const Args &args) {
            args.GetReturnValue().Set(Overlay::CursorShown());
        }
        void GetCursorPositionFn(const Args &args) {
            POINT p {};
            const auto *host = Host();
            if (!Overlay::CursorShown() || !GetCursorPos(&p) || !host) {
                args.GetReturnValue().Set(false);
                return;
            }
            if (HWND window = GetForegroundWindow()) {
                ScreenToClient(window, &p);
            }
            args.GetReturnValue().Set(Vec2(args.GetIsolate(), static_cast<float>(p.x), static_cast<float>(p.y)));
        }

        void OutputChatBoxFn(const Args &args) {
            if (!Has(args, 0)) {
                Throw(args.GetIsolate(), "outputChatBox(text[, r, g, b])");
                return;
            }
            const bool coloured = Has(args, 1) || Has(args, 2) || Has(args, 3);
            const uint32_t argb = ToColor(Num(args, 1, 255), Num(args, 2, 255), Num(args, 3, 255), 255);
            const uint32_t rgba = coloured ? ((argb << 8) | 0xFFu) : 0u;
            Hud::ChatLine(Str(args, 0), rgba);
            if (!Overlay::Ready() && Host()) { // no overlay (yet): the game's subtitle line
                Host()->show_text(static_cast<int>(rdmp::fw::TextStyle::Subtitle), Str(args, 0).c_str(), 6000);
            }
            args.GetReturnValue().Set(true);
        }
        void ShowChatFn(const Args &args) {
            Hud::SetChatVisible(Bool(args, 0, true));
            args.GetReturnValue().Set(true);
        }
        void IsChatVisibleFn(const Args &args) {
            args.GetReturnValue().Set(Hud::ChatVisible());
        }
        void IsChatInputActiveFn(const Args &args) {
            args.GetReturnValue().Set(Hud::ChatInputActive());
        }

        void ShowGameTextFn(const Args &args) {
            if (!Has(args, 0) || !Host()) {
                Throw(args.GetIsolate(), "showGameText(text[, ms, \"subtitle\" | \"big\"])");
                return;
            }
            const auto style = Str(args, 2, "subtitle") == "big" ? rdmp::fw::TextStyle::Big : rdmp::fw::TextStyle::Subtitle;
            Host()->show_text(static_cast<int>(style), Str(args, 0).c_str(), static_cast<int>(Num(args, 1, 4000)));
            args.GetReturnValue().Set(true);
        }

        void SetPlayerNametagShowingFn(const Args &args) {
            const uint64_t id = ElementId(args, 0);
            if (!id) {
                Throw(args.GetIsolate(), "setPlayerNametagShowing(player, showing | null)");
                return;
            }
            Hud::SetNametagOverride(id, Has(args, 1) ? (Bool(args, 1, true) ? 1 : 0) : -1);
            args.GetReturnValue().Set(true);
        }
        void ShowPlayerNametagsFn(const Args &args) {
            Hud::SetNametagsEnabled(Bool(args, 0, true));
            args.GetReturnValue().Set(true);
        }
        void SetNametagDrawDistanceFn(const Args &args) {
            Hud::SetNametagDrawDistance(Float(args, 0, 60.0f));
            args.GetReturnValue().Set(true);
        }
    } // namespace

    void RegisterHudApi(const ScriptApi::Publisher &publish) {
        publish("dxDrawText", &DxDrawTextFn);
        publish("dxDrawRectangle", &DxDrawRectangleFn);
        publish("dxDrawLine", &DxDrawLineFn);
        publish("dxDrawCircle", &DxDrawCircleFn);
        publish("dxGetTextWidth", &DxGetTextWidthFn);
        publish("dxGetFontHeight", &DxGetFontHeightFn);
        publish("guiGetScreenSize", &GuiGetScreenSizeFn);
        publish("getScreenFromWorldPosition", &GetScreenFromWorldPositionFn);
        publish("getCameraPosition", &GetCameraPositionFn);
        publish("showCursor", &ShowCursorFn);
        publish("isCursorShowing", &IsCursorShowingFn);
        publish("getCursorPosition", &GetCursorPositionFn);
        publish("outputChatBox", &OutputChatBoxFn);
        publish("showChat", &ShowChatFn);
        publish("isChatVisible", &IsChatVisibleFn);
        publish("isChatInputActive", &IsChatInputActiveFn);
        publish("showGameText", &ShowGameTextFn);
        publish("setPlayerNametagShowing", &SetPlayerNametagShowingFn);
        publish("showPlayerNametags", &ShowPlayerNametagsFn);
        publish("setNametagDrawDistance", &SetNametagDrawDistanceFn);
    }
} // namespace RDMP::ClientApi
