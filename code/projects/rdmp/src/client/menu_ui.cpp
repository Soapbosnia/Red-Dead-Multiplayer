/*
 * The main menu, drawn with Dear ImGui: see menu_ui.h / frontend.h.
 *
 * Look: ink and leather. A near-black warm backdrop with a low red glow, ivory type, Palatino for headings and the
 * logo, Bahnschrift for everything else, red for the one action that matters on a page, brass for small accents.
 * Every size is written for a 1080 px tall screen and scaled with the window (S).
 */

#include <utils/safe_win32.h>

#include <external/imgui/wrapper.h>
#include <imgui.h>

#include "client/frontend.h"
#include "client/fw_client.h"
#include "client/menu_model.h"
#include "client/menu_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace RDMP::MenuUi {
    namespace {
        namespace M = Frontend::Model;
        namespace opt = rdmp::opt;

        FwClient *g_client = nullptr;

        // ---- palette ----
        constexpr ImU32 Rgba(int r, int g, int b, float a = 1.0f) {
            return IM_COL32(r, g, b, static_cast<int>(a * 255.0f));
        }
        ImVec4 Vec(int r, int g, int b, float a = 1.0f) {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }
        const ImU32 kInk    = Rgba(239, 230, 210);
        const ImU32 kInk2   = Rgba(201, 189, 163);
        const ImU32 kInk3   = Rgba(143, 131, 108);
        const ImU32 kInk4   = Rgba(93, 84, 68);
        const ImU32 kRed    = Rgba(179, 38, 30);
        const ImU32 kRedHi  = Rgba(214, 56, 44);
        const ImU32 kBrass  = Rgba(201, 161, 90);
        const ImU32 kOk     = Rgba(143, 176, 105);
        const ImU32 kWarn   = Rgba(217, 168, 74);
        const ImU32 kLine   = Rgba(239, 230, 210, 0.10f);
        const ImU32 kLine2  = Rgba(239, 230, 210, 0.18f);
        const ImU32 kPanel  = Rgba(20, 15, 11, 0.88f);

        // ---- fonts / scale ----
        ImFont *g_title = nullptr; // Palatino Linotype bold
        ImFont *g_serif = nullptr; // Palatino Linotype
        ImFont *g_icons = nullptr; // Segoe MDL2 Assets
        bool g_fontsLooked = false;
        float S            = 1.0f; // window height / 1080
        float g_themeFor   = 0.0f;

        // ---- state ----
        bool g_inSession  = false;
        bool g_focusNext  = false;

        // Server browser.
        const char *kLists[]      = {"internet", "local", "favorites", "recent"};
        const char *kListLabels[] = {"Internet", "Local", "Favorites", "Recent"};
        int g_list                = 0;
        bool g_listAsked[4]       = {};
        char g_search[64]         = "";
        bool g_hideEmpty = false, g_hideFull = false, g_hideLocked = false;
        std::string g_selected; // "host:port"

        // Join dialogs.
        bool g_askPassword      = false;
        std::string g_joinHost;
        int g_joinPort          = 0;
        char g_joinPassword[64] = "";

        // Direct connect.
        char g_address[128]      = "";
        char g_directPassword[64] = "";

        // Settings.
        enum class Kind { Toggle, Slider, Seg, Resolution, Refresh, Text };
        enum class Fmt { Pct, Meters, Nits, Dec1 };
        struct Item {
            const char *key;
            const char *label;
            const char *hint;
            Kind kind;
            float min = 0, max = 1, step = 0.01f;
            Fmt fmt   = Fmt::Pct;
            std::vector<std::pair<int, const char *>> options = {};
        };
        struct Group {
            const char *title;
            std::vector<Item> items;
        };
        struct Section {
            const char *label;
            unsigned icon;
            std::vector<Group> groups;
        };
        using Opts = std::vector<std::pair<int, const char *>>;
        const Opts kQuality4 = {{0, "Low"}, {1, "Medium"}, {2, "High"}, {3, "Ultra"}};
        const Opts kUpscale  = {{0, "Off"}, {1, "Quality"}, {2, "Balanced"}, {3, "Perf."}, {4, "Ultra perf."}};
        const Opts kOffOn    = {{0, "Off"}, {1, "On"}};
        const Opts kHorse    = {{0, "Camera"}, {1, "Horse"}};
        // clang-format off
        const std::vector<Section> kSections = {
            {"Multiplayer", 0xE716, {
                {"You", {
                    {"mp.nickname", "Nickname", "Shown to other players", Kind::Text},
                }},
                {"Other players", {
                    {"mp.nametags", "Nametags", "Names and health above other players", Kind::Toggle},
                    {"mp.nametagDistance", "Nametag distance", nullptr, Kind::Slider, 10, 200, 5, Fmt::Meters},
                    {"mp.chat", "Chat box", "T opens it in game", Kind::Toggle},
                }},
            }},
            {"Display", 0xE7F4, {
                {"Screen", {
                    {"Fullscreen", "Fullscreen", nullptr, Kind::Toggle},
                    {"@resolution", "Resolution", nullptr, Kind::Resolution},
                    {"RefreshRate", "Refresh rate", nullptr, Kind::Refresh},
                    {"Vsync", "V-Sync", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kOffOn},
                    {"TripleBuffer", "Triple buffering", nullptr, Kind::Toggle},
                    {"FrameRateLimit", "Frame rate limit", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "30"}, {1, "60"}, {2, "Unlimited"}}},
                    {"bFocusPaused", "Pause when unfocused", nullptr, Kind::Toggle},
                    {"bConstrainMouse", "Keep the mouse in the window", nullptr, Kind::Toggle},
                }},
                {"Picture", {
                    {"display.fBrightness", "Brightness", nullptr, Kind::Slider},
                    {"display.fContrast", "Contrast", nullptr, Kind::Slider},
                    {"display.fSaturation", "Saturation", nullptr, Kind::Slider},
                }},
                {"HDR", {
                    {"HDR", "HDR", "Needs an HDR display", Kind::Toggle},
                    {"PeakBrightness", "Peak brightness", nullptr, Kind::Slider, 200, 2000, 50, Fmt::Nits},
                    {"PaperWhite", "Paper white", nullptr, Kind::Slider, 80, 500, 10, Fmt::Nits},
                }},
            }},
            {"Graphics", 0xE790, {
                {"Quality", {
                    {"ShadowQuality", "Shadow quality", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kQuality4},
                    {"ShadowSoftness", "Shadow softness", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kQuality4},
                    {"ShadowBlend", "Shadow blend", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "Low"}, {1, "Medium"}, {2, "High"}}},
                    {"MinAnisotropicFiltering", "Anisotropic filtering", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{1, "1x"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}}},
                    {"aaAntiAliasing", "Anti-aliasing", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "Off"}, {1, "Low"}, {2, "High"}}},
                    {"MotionBlurStyle", "Motion blur", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "Off"}, {1, "Camera"}, {2, "Full"}}},
                    {"MotionBlurStrength", "Motion blur strength", nullptr, Kind::Slider, 0, 1, 0.05f},
                }},
                {"World detail", {
                    {"WorldStreamingRadius", "World streaming distance", nullptr, Kind::Slider, 80, 320, 10, Fmt::Meters},
                    {"TerrainStreamingFactor", "Terrain detail", nullptr, Kind::Slider, 1, 10, 0.5f, Fmt::Dec1},
                    {"TreeLevelOfDetail", "Tree detail", nullptr, Kind::Slider, 0, 4, 0.5f, Fmt::Dec1},
                    {"TreeImposterHighLodStreamingDistance", "Distant trees", nullptr, Kind::Slider, 256, 2048, 64, Fmt::Meters},
                    {"GrassStreamingDistance", "Grass distance", nullptr, Kind::Slider, 50, 600, 10, Fmt::Meters},
                }},
                {"Upscaling and latency", {
                    {"DynamicResolution", "Dynamic resolution", nullptr, Kind::Toggle},
                    {"FSR3UpscalingQuality", "AMD FSR 3", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kUpscale},
                    {"FSR3AdditionalSharpness", "FSR sharpening", nullptr, Kind::Slider, 0, 1, 0.05f},
                    {"DLSSUpscalingQuality", "NVIDIA DLSS", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kUpscale},
                    {"DLSSFrameGeneration", "DLSS frame generation", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kOffOn},
                    {"NVIDIAReflex", "NVIDIA Reflex", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "Off"}, {1, "On"}, {2, "On + Boost"}}},
                }},
            }},
            {"Audio", 0xE767, {
                {"Volume", {
                    {"audio.fSFXVolume", "Sound effects", nullptr, Kind::Slider},
                    {"audio.fMusicVolume", "Music", nullptr, Kind::Slider},
                    {"audio.bHeadphones", "Headphones mix", nullptr, Kind::Toggle},
                }},
            }},
            {"Keyboard & mouse", 0xE765, {
                {"Mouse", {
                    {"kbMouse.fMouseScalarSensitivity", "Mouse sensitivity", nullptr, Kind::Slider},
                    {"kbMouse.fADSScalarSensitivity", "Aiming sensitivity", nullptr, Kind::Slider},
                    {"kbMouse.bInvertedY", "Invert vertical look", nullptr, Kind::Toggle},
                    {"kbMouse.bSwapCamX", "Invert horizontal look", nullptr, Kind::Toggle},
                }},
                {"Movement", {
                    {"kbMouse.bHoldSprint", "Hold to sprint", nullptr, Kind::Toggle},
                    {"kbMouse.bHoldSprintVehicle", "Hold to spur the horse", nullptr, Kind::Toggle},
                    {"kbMouse.iHorseCameraRelativity", "Horse steering", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kHorse},
                }},
            }},
            {"Gamepad", 0xE7FC, {
                {"Sticks", {
                    {"controller.fScalarSensitivity", "Look sensitivity", nullptr, Kind::Slider},
                    {"controller.fLeftStickDeadzone", "Left stick dead zone", nullptr, Kind::Slider, 0, 0.5f},
                    {"controller.fRightStickDeadzone", "Right stick dead zone", nullptr, Kind::Slider, 0, 0.5f},
                    {"controller.bInvertedY", "Invert vertical look", nullptr, Kind::Toggle},
                    {"controller.bSwapCamX", "Invert horizontal look", nullptr, Kind::Toggle},
                    {"controller.bAutoCenter", "Camera auto-center", nullptr, Kind::Toggle},
                    {"controller.bSouthPaw", "Southpaw", nullptr, Kind::Toggle},
                }},
                {"Controller", {
                    {"controller.bRumble", "Vibration", nullptr, Kind::Toggle},
                    {"controller.bSwapButtons", "Swap buttons", nullptr, Kind::Toggle},
                    {"controller.iCombatMode", "Targeting", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, {{0, "Casual"}, {1, "Normal"}, {2, "Expert"}}},
                    {"controller.iHorseCameraRelativity", "Horse steering", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kHorse},
                }},
            }},
            {"Interface", 0xE81E, {
                {"HUD", {
                    {"display.bRadarOn", "Radar", nullptr, Kind::Toggle},
                    {"display.bRouteOn", "GPS route", nullptr, Kind::Toggle},
                    {"display.Subtitles", "Subtitles", nullptr, Kind::Seg, 0, 1, 1, Fmt::Pct, kOffOn},
                    {"display.fSubtitleScale", "Subtitle size", nullptr, Kind::Slider, 0.5f, 1.5f, 0.05f},
                    {"display.fObjectiveScale", "Objective text size", nullptr, Kind::Slider, 0.5f, 1.5f, 0.05f},
                    {"display.fHelpScale", "Help text size", nullptr, Kind::Slider, 0.5f, 1.5f, 0.05f},
                }},
            }},
        };
        // clang-format on
        int g_section = 0;
        M::MpSettings g_mpLive, g_mpStage;
        bool g_gameAvailable = false;
        M::GameBlocks g_gameLive, g_gameStage;
        char g_nickname[32] = "";
        // Display / graphics changes revert unless kept.
        bool g_keepAsk      = false;
        uint64_t g_keepUntil = 0;
        M::GameBlocks g_keepBefore;

        uint64_t NowMs() {
            return GetTickCount64();
        }

        // =============================================================================================================
        // theme
        // =============================================================================================================
        void LookUpFonts() {
            if (g_fontsLooked || !g_client || !g_client->GetImGUI()) {
                return;
            }
            g_fontsLooked = true;
            auto *imgui   = g_client->GetImGUI();
            g_title       = imgui->GetFont("title");
            g_serif       = imgui->GetFont("serif");
            g_icons       = imgui->GetFont("icons");
        }

        void ApplyTheme() {
            if (std::fabs(g_themeFor - S) < 0.001f) {
                return;
            }
            g_themeFor = S;
            ImGuiStyle st;
            st.WindowPadding     = ImVec2(20, 20);
            st.WindowRounding    = 6;
            st.WindowBorderSize  = 1;
            st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
            st.ChildRounding     = 6;
            st.ChildBorderSize   = 1;
            st.PopupRounding     = 6;
            st.PopupBorderSize   = 1;
            st.FramePadding      = ImVec2(12, 8);
            st.FrameRounding     = 4;
            st.FrameBorderSize   = 1;
            st.ItemSpacing       = ImVec2(10, 10);
            st.ItemInnerSpacing  = ImVec2(8, 6);
            st.CellPadding       = ImVec2(10, 9);
            st.ScrollbarSize     = 10;
            st.ScrollbarRounding = 4;
            st.GrabMinSize       = 14;
            st.GrabRounding      = 4;
            st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
            ImVec4 *c                         = st.Colors;
            c[ImGuiCol_Text]                  = Vec(239, 230, 210);
            c[ImGuiCol_TextDisabled]          = Vec(143, 131, 108);
            c[ImGuiCol_WindowBg]              = Vec(16, 12, 9, 0.97f);
            c[ImGuiCol_ChildBg]               = Vec(0, 0, 0, 0);
            c[ImGuiCol_PopupBg]               = Vec(22, 17, 13, 0.98f);
            c[ImGuiCol_Border]                = Vec(239, 230, 210, 0.12f);
            c[ImGuiCol_BorderShadow]          = Vec(0, 0, 0, 0);
            c[ImGuiCol_FrameBg]               = Vec(239, 230, 210, 0.05f);
            c[ImGuiCol_FrameBgHovered]        = Vec(239, 230, 210, 0.09f);
            c[ImGuiCol_FrameBgActive]         = Vec(239, 230, 210, 0.12f);
            c[ImGuiCol_TitleBg]               = Vec(12, 9, 7, 0.98f);
            c[ImGuiCol_TitleBgActive]         = Vec(28, 19, 13, 0.98f);
            c[ImGuiCol_TitleBgCollapsed]      = Vec(12, 9, 7, 0.98f);
            c[ImGuiCol_Button]                = Vec(239, 230, 210, 0.07f);
            c[ImGuiCol_ButtonHovered]         = Vec(239, 230, 210, 0.13f);
            c[ImGuiCol_ButtonActive]          = Vec(239, 230, 210, 0.18f);
            c[ImGuiCol_Header]                = Vec(179, 38, 30, 0.22f);
            c[ImGuiCol_HeaderHovered]         = Vec(239, 230, 210, 0.07f);
            c[ImGuiCol_HeaderActive]          = Vec(179, 38, 30, 0.32f);
            c[ImGuiCol_CheckMark]             = Vec(214, 56, 44);
            c[ImGuiCol_SliderGrab]            = Vec(214, 56, 44);
            c[ImGuiCol_SliderGrabActive]      = Vec(235, 80, 66);
            c[ImGuiCol_ScrollbarBg]           = Vec(0, 0, 0, 0);
            c[ImGuiCol_ScrollbarGrab]         = Vec(239, 230, 210, 0.14f);
            c[ImGuiCol_ScrollbarGrabHovered]  = Vec(239, 230, 210, 0.22f);
            c[ImGuiCol_ScrollbarGrabActive]   = Vec(239, 230, 210, 0.30f);
            c[ImGuiCol_Separator]             = Vec(239, 230, 210, 0.10f);
            c[ImGuiCol_TableHeaderBg]         = Vec(0, 0, 0, 0);
            c[ImGuiCol_TableBorderStrong]     = Vec(239, 230, 210, 0.10f);
            c[ImGuiCol_TableBorderLight]      = Vec(239, 230, 210, 0.06f);
            c[ImGuiCol_TableRowBg]            = Vec(0, 0, 0, 0);
            c[ImGuiCol_TableRowBgAlt]         = Vec(239, 230, 210, 0.025f);
            c[ImGuiCol_TextSelectedBg]        = Vec(179, 38, 30, 0.45f);
            c[ImGuiCol_ModalWindowDimBg]      = Vec(5, 4, 3, 0.70f);
            c[ImGuiCol_ResizeGrip]            = Vec(239, 230, 210, 0.06f);
            c[ImGuiCol_ResizeGripHovered]     = Vec(214, 56, 44, 0.5f);
            c[ImGuiCol_ResizeGripActive]      = Vec(214, 56, 44, 0.8f);
            st.ScaleAllSizes(S);
            st.FontSizeBase = 18.0f;
            st.FontScaleDpi = S;
            ImGui::GetStyle() = st;
        }

        // =============================================================================================================
        // drawing helpers
        // =============================================================================================================
        std::string Icon(unsigned cp) {
            char out[4] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                           static_cast<char>(0x80 | (cp & 0x3F)), 0};
            return out;
        }

        ImFont *Sans() {
            return ImGui::GetIO().Fonts->Fonts.empty() ? ImGui::GetFont() : ImGui::GetIO().Fonts->Fonts[0];
        }
        ImFont *Or(ImFont *font) {
            return font ? font : Sans();
        }

        ImVec2 Measure(ImFont *font, float size, const char *text) {
            return Or(font)->CalcTextSizeA(size * S, FLT_MAX, 0.0f, text);
        }
        void Put(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text) {
            dl->AddText(Or(font), size * S, pos, col, text);
        }
        // Letter-spaced text (the logo, small capitals); returns its width.
        float PutSpaced(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float spacing, bool measureOnly = false) {
            float x = pos.x;
            for (const char *p = text; *p; ++p) {
                const char ch[2] = {*p, 0};
                if (!measureOnly) {
                    dl->AddText(Or(font), size * S, ImVec2(x, pos.y), col, ch);
                }
                x += Measure(font, size, ch).x + spacing * S;
            }
            return x - pos.x - (*text ? spacing * S : 0.0f);
        }
        void PutIcon(ImDrawList *dl, float size, ImVec2 pos, ImU32 col, unsigned cp) {
            if (g_icons) {
                dl->AddText(g_icons, size * S, pos, col, Icon(cp).c_str());
            }
        }

        void Label(ImFont *font, float size, ImU32 col, const char *text, float wrap = 0.0f) {
            ImGui::PushFont(Or(font), size);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (wrap > 0.0f) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
            }
            else {
                ImGui::TextUnformatted(text);
            }
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        void Gap(float px) {
            ImGui::Dummy(ImVec2(0, px * S));
        }

        void RadialGlow(ImDrawList *dl, ImVec2 c, float radius, int r, int g, int b, float alpha) {
            constexpr int kSteps = 36;
            for (int i = 0; i < kSteps; ++i) {
                const float t = 1.0f - static_cast<float>(i) / kSteps;
                dl->AddCircleFilled(c, radius * t, Rgba(r, g, b, alpha / kSteps * 1.6f), 64);
            }
        }

        void Spinner(ImDrawList *dl, ImVec2 c, float radius, ImU32 col) {
            const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
            dl->PathClear();
            dl->PathArcTo(c, radius, t, t + 3.14159265f * 1.35f, 32);
            dl->PathStroke(col, 0, 3.0f * S);
        }

        enum class Tone { Primary, Secondary, Ghost, Danger };
        // A text button in the menu's look; `icon` 0 = none.
        bool Button(const char *id, const char *text, ImVec2 size, Tone tone = Tone::Secondary, unsigned icon = 0, bool enabled = true) {
            ImGui::PushID(id);
            const float fontSize = 18.0f;
            const ImVec2 textSize = Measure(nullptr, fontSize, text);
            const float iconW     = icon && g_icons ? 26.0f * S : 0.0f;
            if (size.x <= 0) {
                size.x = textSize.x + iconW + 40.0f * S;
            }
            if (size.y <= 0) {
                size.y = 44.0f * S;
            }
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(!enabled);
            const bool pressed = ImGui::InvisibleButton("##b", size);
            ImGui::EndDisabled();
            const bool hovered = enabled && ImGui::IsItemHovered();
            const bool held    = enabled && ImGui::IsItemActive();
            ImDrawList *dl     = ImGui::GetWindowDrawList();
            ImU32 bg = 0, border = 0, fg = enabled ? kInk : kInk4;
            switch (tone) {
            case Tone::Primary:
                bg     = !enabled ? Rgba(179, 38, 30, 0.25f) : held ? Rgba(150, 30, 24) : hovered ? kRedHi : kRed;
                border = 0;
                fg     = enabled ? Rgba(255, 246, 232) : kInk3;
                break;
            case Tone::Secondary:
                bg     = held ? Rgba(239, 230, 210, 0.16f) : hovered ? Rgba(239, 230, 210, 0.11f) : Rgba(239, 230, 210, 0.06f);
                border = kLine2;
                break;
            case Tone::Ghost:
                bg = held ? Rgba(239, 230, 210, 0.10f) : hovered ? Rgba(239, 230, 210, 0.06f) : 0;
                fg = enabled ? (hovered ? kInk : kInk2) : kInk4;
                break;
            case Tone::Danger:
                bg     = held ? Rgba(179, 38, 30, 0.35f) : hovered ? Rgba(179, 38, 30, 0.25f) : Rgba(179, 38, 30, 0.12f);
                border = Rgba(214, 56, 44, 0.45f);
                break;
            }
            const ImVec2 end = ImVec2(pos.x + size.x, pos.y + size.y);
            if (bg) {
                dl->AddRectFilled(pos, end, bg, 4.0f * S);
            }
            if (border) {
                dl->AddRect(pos, end, border, 4.0f * S, 0, 1.0f);
            }
            const float contentW = textSize.x + iconW;
            float x              = pos.x + (size.x - contentW) * 0.5f;
            const float y        = pos.y + (size.y - textSize.y) * 0.5f;
            if (iconW > 0) {
                PutIcon(dl, 16.0f, ImVec2(x, pos.y + (size.y - 16.0f * S) * 0.5f), fg, icon);
                x += iconW;
            }
            Put(dl, nullptr, fontSize, ImVec2(x, y), fg, text);
            ImGui::PopID();
            return pressed && enabled;
        }

        // An on / off switch.
        bool Toggle(const char *id, bool *value) {
            const ImVec2 pos  = ImGui::GetCursorScreenPos();
            const ImVec2 size = ImVec2(48.0f * S, 26.0f * S);
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##t", size);
            ImGui::PopID();
            if (pressed) {
                *value = !*value;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *dl     = ImGui::GetWindowDrawList();
            const float r      = size.y * 0.5f;
            const ImU32 track  = *value ? (hovered ? kRedHi : kRed) : (hovered ? Rgba(239, 230, 210, 0.20f) : Rgba(239, 230, 210, 0.12f));
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), track, r);
            const float knobX = *value ? pos.x + size.x - r : pos.x + r;
            dl->AddCircleFilled(ImVec2(knobX, pos.y + r), r - 4.0f * S, *value ? Rgba(255, 246, 232) : kInk2, 24);
            return pressed;
        }

        // Segmented choice; returns true when the value changed.
        bool Segmented(const char *id, int *value, const Opts &options, float width) {
            ImGui::PushID(id);
            bool changed     = false;
            const float h    = 34.0f * S;
            const float each = width / static_cast<float>(options.size());
            ImDrawList *dl   = ImGui::GetWindowDrawList();
            const ImVec2 o   = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(o, ImVec2(o.x + width, o.y + h), Rgba(239, 230, 210, 0.05f), 4.0f * S);
            dl->AddRect(o, ImVec2(o.x + width, o.y + h), kLine, 4.0f * S);
            for (size_t i = 0; i < options.size(); ++i) {
                const ImVec2 p = ImVec2(o.x + each * i, o.y);
                ImGui::SetCursorScreenPos(p);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::InvisibleButton("##s", ImVec2(each, h))) {
                    if (*value != options[i].first) {
                        *value  = options[i].first;
                        changed = true;
                    }
                }
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = *value == options[i].first;
                if (on) {
                    dl->AddRectFilled(ImVec2(p.x + 2 * S, p.y + 2 * S), ImVec2(p.x + each - 2 * S, p.y + h - 2 * S), kRed, 3.0f * S);
                }
                else if (hovered) {
                    dl->AddRectFilled(ImVec2(p.x + 2 * S, p.y + 2 * S), ImVec2(p.x + each - 2 * S, p.y + h - 2 * S), Rgba(239, 230, 210, 0.07f), 3.0f * S);
                }
                const ImVec2 ts = Measure(nullptr, 15.0f, options[i].second);
                Put(dl, nullptr, 15.0f, ImVec2(p.x + (each - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), on ? Rgba(255, 246, 232) : hovered ? kInk : kInk2,
                    options[i].second);
            }
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h));
            ImGui::Dummy(ImVec2(width, 0));
            ImGui::PopID();
            return changed;
        }

        // A panel: dark leather, a hairline border.
        bool BeginCard(const char *id, ImVec2 size, bool scroll = false) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kPanel));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kLine));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * S, 22.0f * S));
            const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                                scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            return open;
        }
        void EndCard() {
            ImGui::EndChild();
        }

        void CardTitle(const char *text, unsigned icon = 0) {
            ImDrawList *dl   = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            float x          = pos.x;
            if (icon && g_icons) {
                PutIcon(dl, 15.0f, ImVec2(x, pos.y + 2.0f * S), kBrass, icon);
                x += 26.0f * S;
            }
            std::string upper = text;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            PutSpaced(dl, nullptr, 14.0f, ImVec2(x, pos.y), kBrass, upper.c_str(), 2.5f);
            ImGui::Dummy(ImVec2(0, 20.0f * S));
            Gap(4);
        }

        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string AddressOf(const ServerQuery::Info &info) {
            return info.host + ":" + std::to_string(info.port);
        }

        std::string NameOf(const ServerQuery::Info &info) {
            return info.name.empty() ? AddressOf(info) : info.name;
        }

        ImU32 PingColor(int ping) {
            return ping < 0 ? kInk4 : ping < 80 ? kOk : ping < 150 ? kWarn : kRedHi;
        }

        // =============================================================================================================
        // joining
        // =============================================================================================================
        void Join(const ServerQuery::Info &info) {
            if (info.password) {
                g_askPassword     = true;
                g_joinHost        = info.host;
                g_joinPort        = info.port;
                g_joinPassword[0] = 0;
                return;
            }
            M::Connect(info.host, info.port, "");
        }

        void ServersPage(ImVec2 avail) {
            const float startY = ImGui::GetCursorPosY();
            ImDrawList *dl     = ImGui::GetWindowDrawList();

            // Tabs.
            const ImVec2 tabsAt = ImGui::GetCursorScreenPos();
            float tx            = tabsAt.x;
            for (int i = 0; i < 4; ++i) {
                const ImVec2 ts = Measure(nullptr, 19.0f, kListLabels[i]);
                ImGui::SetCursorScreenPos(ImVec2(tx, tabsAt.y));
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("##tab", ImVec2(ts.x + 28.0f * S, 40.0f * S))) {
                    g_list = i;
                    if (!g_listAsked[i]) {
                        g_listAsked[i] = true;
                        M::Refresh(kLists[i]);
                    }
                }
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = g_list == i;
                Put(dl, nullptr, 19.0f, ImVec2(tx + 14.0f * S, tabsAt.y + 8.0f * S), on ? kInk : hovered ? kInk2 : kInk3, kListLabels[i]);
                if (on) {
                    dl->AddRectFilled(ImVec2(tx + 14.0f * S, tabsAt.y + 37.0f * S), ImVec2(tx + 14.0f * S + ts.x, tabsAt.y + 40.0f * S), kRedHi);
                }
                tx += ts.x + 28.0f * S;
            }
            // Search + refresh on the right of the tabs.
            const float searchW = 300.0f * S;
            const float refW    = 130.0f * S;
            ImGui::SetCursorScreenPos(ImVec2(tabsAt.x + avail.x - searchW - refW - 12.0f * S, tabsAt.y));
            ImGui::SetNextItemWidth(searchW);
            ImGui::InputTextWithHint("##search", "Search servers", g_search, sizeof(g_search));
            ImGui::SameLine(0, 12.0f * S);
            const std::string list = kLists[g_list];
            if (Button("refresh", "Refresh", ImVec2(refW, ImGui::GetFrameHeight()), Tone::Secondary, 0xE72C)) {
                M::Refresh(list);
            }
            ImGui::SetCursorScreenPos(ImVec2(tabsAt.x, tabsAt.y + 41.0f * S));
            dl->AddLine(ImGui::GetCursorScreenPos(), ImVec2(tabsAt.x + avail.x, tabsAt.y + 41.0f * S), kLine, 1.0f);
            Gap(12);
            ImGui::Checkbox("Hide empty", &g_hideEmpty);
            ImGui::SameLine(0, 24.0f * S);
            ImGui::Checkbox("Hide full", &g_hideFull);
            ImGui::SameLine(0, 24.0f * S);
            ImGui::Checkbox("Hide locked", &g_hideLocked);
            Gap(4);

            // Rows that pass the filters.
            const auto &rows         = M::List(list);
            const std::string needle = ToLower(g_search);
            std::vector<const M::Server *> shown;
            for (const auto &row : rows) {
                const auto &info = row.info;
                if (!needle.empty() && ToLower(info.name).find(needle) == std::string::npos && ToLower(AddressOf(info)).find(needle) == std::string::npos) {
                    continue;
                }
                if (row.answered && info.online) {
                    if ((g_hideEmpty && info.players == 0) || (g_hideFull && info.maxPlayers > 0 && info.players >= info.maxPlayers) ||
                        (g_hideLocked && info.password)) {
                        continue;
                    }
                }
                shown.push_back(&row);
            }

            const float barH   = 84.0f * S;
            const float tableH = avail.y - (ImGui::GetCursorPosY() - startY) - barH - ImGui::GetStyle().ItemSpacing.y;
            const M::Server *selected = nullptr;
            if (BeginCard("##list", ImVec2(avail.x, tableH), false)) {
                const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable |
                                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
                if (ImGui::BeginTable("##servers", 5, flags)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 30.0f * S);
                    ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
                    ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 120.0f * S);
                    ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 100.0f * S);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 30.0f * S);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk3));
                    ImGui::TableHeadersRow();
                    ImGui::PopStyleColor();
                    if (ImGuiTableSortSpecs *sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs spec = sort->Specs[0];
                        std::stable_sort(shown.begin(), shown.end(), [&](const M::Server *a, const M::Server *b) {
                            int d = 0;
                            if (spec.ColumnIndex == 1) d = ToLower(NameOf(a->info)).compare(ToLower(NameOf(b->info)));
                            else if (spec.ColumnIndex == 2) d = a->info.players - b->info.players;
                            else if (spec.ColumnIndex == 3) {
                                const int pa = a->info.pingMs < 0 ? 99999 : a->info.pingMs, pb = b->info.pingMs < 0 ? 99999 : b->info.pingMs;
                                d = pa - pb;
                            }
                            return spec.SortDirection == ImGuiSortDirection_Descending ? d > 0 : d < 0;
                        });
                    }
                    for (const M::Server *row : shown) {
                        const auto &info       = row->info;
                        const std::string addr = AddressOf(info);
                        ImGui::TableNextRow(0, 48.0f * S);
                        ImGui::TableSetColumnIndex(0);
                        const bool fav = M::IsFavorite(info.host, info.port);
                        if (g_icons) {
                            ImGui::PushFont(g_icons, 15.0f);
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fav ? kBrass : kInk4));
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(Icon(fav ? 0xE735 : 0xE734).c_str());
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        }
                        ImGui::TableSetColumnIndex(1);
                        const bool isSel = g_selected == addr;
                        if (isSel) {
                            selected = row;
                        }
                        const ImVec2 cell = ImGui::GetCursorScreenPos();
                        ImGui::PushID(addr.c_str());
                        if (ImGui::Selectable("##sel", isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                                                    ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 48.0f * S))) {
                            g_selected = addr;
                            selected   = row;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (!row->answered || info.online)) {
                                Join(info);
                            }
                        }
                        ImGui::PopID();
                        ImDrawList *tdl   = ImGui::GetWindowDrawList();
                        const bool off    = row->answered && !info.online;
                        Put(tdl, nullptr, 17.5f, ImVec2(cell.x, cell.y + 3.0f * S), off ? kInk3 : kInk, NameOf(info).c_str());
                        Put(tdl, nullptr, 13.0f, ImVec2(cell.x, cell.y + 26.0f * S), kInk4, (addr + (info.version.empty() ? "" : "   ·   " + info.version)).c_str());

                        ImGui::TableSetColumnIndex(2);
                        ImGui::AlignTextToFramePadding();
                        if (row->answered && info.online) {
                            const std::string players = std::to_string(info.players) + " / " + std::to_string(info.maxPlayers);
                            ImGui::TextUnformatted(players.c_str());
                        }
                        else {
                            ImGui::TextDisabled("-");
                        }
                        ImGui::TableSetColumnIndex(3);
                        ImGui::AlignTextToFramePadding();
                        if (!row->answered) {
                            ImGui::TextDisabled("...");
                        }
                        else if (!info.online) {
                            ImGui::TextDisabled("offline");
                        }
                        else {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PingColor(info.pingMs)));
                            ImGui::Text("%d ms", info.pingMs);
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableSetColumnIndex(4);
                        if (info.password && g_icons) {
                            ImGui::PushFont(g_icons, 14.0f);
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk3));
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(Icon(0xE72E).c_str());
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        }
                    }
                    ImGui::EndTable();
                }
                // Empty states.
                if (shown.empty()) {
                    const char *text = nullptr;
                    const std::string err = M::ListError(list);
                    if (!err.empty()) text = err.c_str();
                    else if (M::Refreshing(list)) text = "Looking for servers...";
                    else if (list == "favorites") text = rows.empty() ? "No favourites yet. Star a server to keep it here." : "No servers match the filters.";
                    else if (list == "recent") text = rows.empty() ? "Servers you join show up here." : "No servers match the filters.";
                    else if (list == "local") text = "No server is running on this PC (ports 27015 - 27020).";
                    else text = rows.empty() ? "No servers found." : "No servers match the filters.";
                    const ImVec2 p  = ImGui::GetWindowPos();
                    const ImVec2 sz = ImGui::GetWindowSize();
                    const ImVec2 ts = Measure(nullptr, 17.0f, text);
                    if (M::Refreshing(list) && err.empty()) {
                        Spinner(ImGui::GetWindowDrawList(), ImVec2(p.x + sz.x * 0.5f, p.y + sz.y * 0.5f - 34.0f * S), 14.0f * S, kRedHi);
                    }
                    Put(ImGui::GetWindowDrawList(), nullptr, 17.0f, ImVec2(p.x + (sz.x - ts.x) * 0.5f, p.y + sz.y * 0.5f), err.empty() ? kInk3 : kRedHi, text);
                }
            }
            EndCard();

            // The selected server.
            if (BeginCard("##selected", ImVec2(avail.x, barH), false)) {
                const float innerH = ImGui::GetContentRegionAvail().y;
                if (selected) {
                    const auto &info = selected->info;
                    ImGui::BeginGroup();
                    Label(nullptr, 19.0f, kInk, NameOf(info).c_str());
                    std::string meta = AddressOf(info);
                    if (selected->answered && info.online) {
                        meta += "   ·   " + std::to_string(info.players) + " / " + std::to_string(info.maxPlayers) + " players   ·   " +
                                std::to_string(info.pingMs) + " ms" + (info.password ? "   ·   password" : "");
                    }
                    Label(nullptr, 14.0f, kInk3, meta.c_str());
                    ImGui::EndGroup();
                    const bool fav     = M::IsFavorite(info.host, info.port);
                    const float joinW  = 150.0f * S;
                    const float favW   = 160.0f * S;
                    const float bh     = 42.0f * S;
                    const float bx     = ImGui::GetWindowContentRegionMax().x - joinW - favW - 12.0f * S;
                    ImGui::SameLine(bx);
                    ImGui::SetCursorPosY((ImGui::GetWindowHeight() - bh) * 0.5f);
                    if (Button("fav", fav ? "Unfavourite" : "Favourite", ImVec2(favW, bh), Tone::Secondary, fav ? 0xE735 : 0xE734)) {
                        M::SetFavorite(info.host, info.port, info.name, !fav);
                    }
                    ImGui::SameLine(0, 12.0f * S);
                    if (Button("join", "Join", ImVec2(joinW, bh), Tone::Primary, 0xE768, !selected->answered || info.online)) {
                        Join(info);
                    }
                }
                else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (innerH - 20.0f * S) * 0.5f - 4.0f * S);
                    const std::string count = std::to_string(shown.size()) + (shown.size() == 1 ? " server" : " servers") + "  ·  select one to see it here";
                    Label(nullptr, 16.0f, kInk3, count.c_str());
                }
            }
            EndCard();
        }

        // ---- settings ----
        bool IsMp(const Item &item) {
            return std::strncmp(item.key, "mp.", 3) == 0;
        }

        bool ItemChanged(const Item &item) {
            if (IsMp(item)) {
                if (!std::strcmp(item.key, "mp.nickname")) return M::CleanNickname(g_nickname) != g_mpLive.nickname;
                if (!std::strcmp(item.key, "mp.nametags")) return g_mpStage.nametags != g_mpLive.nametags;
                if (!std::strcmp(item.key, "mp.nametagDistance")) return g_mpStage.nametagDistance != g_mpLive.nametagDistance;
                if (!std::strcmp(item.key, "mp.chat")) return g_mpStage.chat != g_mpLive.chat;
                return false;
            }
            if (!g_gameAvailable) {
                return false;
            }
            auto differs = [](const char *key) {
                const opt::Field *f = M::FindField(key);
                return f && M::GetField(*f, g_gameStage) != M::GetField(*f, g_gameLive);
            };
            if (item.kind == Kind::Resolution) {
                return differs("ResolutionX") || differs("ResolutionY");
            }
            return differs(item.key);
        }

        int SectionChanges(const Section &section) {
            int n = 0;
            for (const auto &g : section.groups) {
                for (const auto &item : g.items) {
                    n += ItemChanged(item) ? 1 : 0;
                }
            }
            return n;
        }

        std::string FormatValue(const Item &item, double v) {
            char buf[32];
            switch (item.fmt) {
            case Fmt::Pct: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(v * 100.0))); break;
            case Fmt::Meters: std::snprintf(buf, sizeof(buf), "%d m", static_cast<int>(std::lround(v))); break;
            case Fmt::Nits: std::snprintf(buf, sizeof(buf), "%d nits", static_cast<int>(std::lround(v))); break;
            case Fmt::Dec1: std::snprintf(buf, sizeof(buf), "%.1f", v); break;
            }
            return buf;
        }

        // A slider over [min, max] snapped to `step`, its value shown in the item's format.
        bool Slider(const Item &item, double &value, float width) {
            float v                  = static_cast<float>(value);
            const std::string format = FormatValue(item, v);
            ImGui::SetNextItemWidth(width);
            ImGui::PushID(item.key);
            const bool changed = ImGui::SliderFloat("##v", &v, item.min, item.max, format.c_str(), ImGuiSliderFlags_NoInput);
            ImGui::PopID();
            if (changed) {
                const float snapped = item.step > 0 ? item.min + std::round((v - item.min) / item.step) * item.step : v;
                value               = std::clamp(snapped, item.min, item.max);
            }
            return changed;
        }

        void SettingControl(const Item &item, float width) {
            // Multiplayer.
            if (!std::strcmp(item.key, "mp.nickname")) {
                ImGui::SetNextItemWidth(width);
                ImGui::InputText("##nick", g_nickname, 23);
                return;
            }
            if (!std::strcmp(item.key, "mp.nametags")) {
                Toggle(item.key, &g_mpStage.nametags);
                return;
            }
            if (!std::strcmp(item.key, "mp.chat")) {
                Toggle(item.key, &g_mpStage.chat);
                return;
            }
            if (!std::strcmp(item.key, "mp.nametagDistance")) {
                double v = g_mpStage.nametagDistance;
                if (Slider(item, v, width)) {
                    g_mpStage.nametagDistance = static_cast<int>(std::lround(v));
                }
                return;
            }
            // The game's.
            if (item.kind == Kind::Resolution) {
                const opt::Field *fx = M::FindField("ResolutionX");
                const opt::Field *fy = M::FindField("ResolutionY");
                const int cx         = static_cast<int>(M::GetField(*fx, g_gameStage));
                const int cy         = static_cast<int>(M::GetField(*fy, g_gameStage));
                char current[32];
                std::snprintf(current, sizeof(current), "%d x %d", cx, cy);
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo("##res", current)) {
                    for (const auto &[w, h] : M::Resolutions()) {
                        char text[32];
                        std::snprintf(text, sizeof(text), "%d x %d", w, h);
                        if (ImGui::Selectable(text, w == cx && h == cy)) {
                            M::SetField(*fx, g_gameStage, w);
                            M::SetField(*fy, g_gameStage, h);
                        }
                    }
                    ImGui::EndCombo();
                }
                return;
            }
            const opt::Field *f = M::FindField(item.key);
            if (!f) {
                return;
            }
            double v = M::GetField(*f, g_gameStage);
            switch (item.kind) {
            case Kind::Toggle: {
                bool b = v != 0.0;
                if (Toggle(item.key, &b)) {
                    M::SetField(*f, g_gameStage, b ? 1 : 0);
                }
            } break;
            case Kind::Slider:
                if (Slider(item, v, width)) {
                    M::SetField(*f, g_gameStage, v);
                }
                break;
            case Kind::Seg: {
                int i = static_cast<int>(std::lround(v));
                if (Segmented(item.key, &i, item.options, width)) {
                    M::SetField(*f, g_gameStage, i);
                }
            } break;
            case Kind::Refresh: {
                const int cur = static_cast<int>(std::lround(v));
                char current[24];
                std::snprintf(current, sizeof(current), "%d Hz", cur);
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo("##hz", current)) {
                    for (int hz : M::RefreshRates()) {
                        char text[24];
                        std::snprintf(text, sizeof(text), "%d Hz", hz);
                        if (ImGui::Selectable(text, hz == cur)) {
                            M::SetField(*f, g_gameStage, hz);
                        }
                    }
                    ImGui::EndCombo();
                }
            } break;
            default: break;
            }
        }

        void ApplySettings() {
            g_mpStage.nickname = M::CleanNickname(g_nickname);
            if (!(g_mpStage == g_mpLive)) {
                M::SaveMp(g_mpStage);
                g_mpLive = g_mpStage;
                std::snprintf(g_nickname, sizeof(g_nickname), "%s", g_mpLive.nickname.c_str());
            }
            if (g_gameAvailable) {
                bool loadedChanged = false, graphicsChanged = false;
                const M::GameBlocks before = g_gameLive;
                if (!M::ApplyGame(before, g_gameStage, loadedChanged, graphicsChanged)) {
                    M::Notify(true, "The game's settings are not available right now.");
                    return;
                }
                g_gameAvailable = M::ReadGame(g_gameLive);
                g_gameStage     = g_gameLive;
                if (graphicsChanged) {
                    g_keepAsk    = true;
                    g_keepUntil  = NowMs() + 15000;
                    g_keepBefore = before;
                    return;
                }
            }
            M::Notify(false, "Settings saved.");
        }

        void RevertDisplay() {
            bool l = false, g = false;
            M::GameBlocks now;
            if (M::ReadGame(now)) {
                M::GameBlocks restore = now;
                std::memcpy(restore.graphics, g_keepBefore.graphics, sizeof(restore.graphics));
                M::ApplyGame(now, restore, l, g);
            }
            g_gameAvailable = M::ReadGame(g_gameLive);
            g_gameStage     = g_gameLive;
            g_keepAsk       = false;
            M::Notify(false, "Display settings reverted.");
        }

        void SettingsPage(ImVec2 avail) {
            const float listW = 230.0f * S;
            const float top   = ImGui::GetCursorPosY();
            const float footH = 52.0f * S;
            const float h     = avail.y - footH - 12.0f * S;

            // Sections.
            ImGui::BeginGroup();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < static_cast<int>(kSections.size()); ++i) {
                if (i > 0 && !g_gameAvailable) {
                    break;
                }
                const Section &sec = kSections[i];
                ImGui::PushID(i);
                const ImVec2 p     = ImGui::GetCursorScreenPos();
                const bool pressed = ImGui::InvisibleButton("##sec", ImVec2(listW, 42.0f * S));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (pressed) {
                    g_section = i;
                }
                const bool on = g_section == i;
                if (on) {
                    dl->AddRectFilled(p, ImVec2(p.x + listW, p.y + 42.0f * S), Rgba(239, 230, 210, 0.07f), 4.0f * S);
                    dl->AddRectFilled(p, ImVec2(p.x + 3.0f * S, p.y + 42.0f * S), kRedHi);
                }
                else if (hovered) {
                    dl->AddRectFilled(p, ImVec2(p.x + listW, p.y + 42.0f * S), Rgba(239, 230, 210, 0.04f), 4.0f * S);
                }
                PutIcon(dl, 16.0f, ImVec2(p.x + 16.0f * S, p.y + 13.0f * S), on ? kRedHi : kInk3, sec.icon);
                Put(dl, nullptr, 17.0f, ImVec2(p.x + 46.0f * S, p.y + 10.0f * S), on || hovered ? kInk : kInk2, sec.label);
                if (const int n = SectionChanges(sec); n > 0) {
                    const std::string badge = std::to_string(n);
                    const ImVec2 ts         = Measure(nullptr, 13.0f, badge.c_str());
                    const ImVec2 c          = ImVec2(p.x + listW - 22.0f * S, p.y + 21.0f * S);
                    dl->AddCircleFilled(c, 10.0f * S, kBrass, 16);
                    Put(dl, nullptr, 13.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), Rgba(20, 15, 11), badge.c_str());
                }
            }
            if (!g_gameAvailable) {
                Gap(8);
                Label(nullptr, 14.0f, kInk4, "The game's own settings are not available right now.", listW);
            }
            ImGui::EndGroup();

            // The section's items.
            ImGui::SameLine(0, 24.0f * S);
            ImGui::SetCursorPosY(top);
            const float paneW = avail.x - listW - 24.0f * S;
            if (g_section >= static_cast<int>(kSections.size()) || (!g_gameAvailable && g_section > 0)) {
                g_section = 0;
            }
            if (BeginCard("##settings", ImVec2(paneW, h), true)) {
                const Section &sec   = kSections[g_section];
                const float controlW = std::min(400.0f * S, paneW * 0.48f);
                bool firstGroup      = true;
                for (const auto &group : sec.groups) {
                    if (!firstGroup) {
                        Gap(16);
                    }
                    firstGroup = false;
                    CardTitle(group.title);
                    for (const auto &item : group.items) {
                        ImGui::PushID(item.key);
                        const float rowY    = ImGui::GetCursorPosY();
                        const float rowH    = 54.0f * S;
                        const float labelY  = item.hint ? rowY + 5.0f * S : rowY + 15.0f * S;
                        ImGui::SetCursorPosY(labelY);
                        Label(nullptr, 18.0f, ItemChanged(item) ? kBrass : kInk, item.label);
                        if (item.hint) {
                            Label(nullptr, 14.0f, kInk3, item.hint);
                        }
                        // The control, right-aligned and centred on the row.
                        const float ctrlH = item.kind == Kind::Toggle ? 26.0f * S : item.kind == Kind::Seg ? 34.0f * S : ImGui::GetFrameHeight();
                        const float ctrlW = item.kind == Kind::Toggle ? 48.0f * S : controlW;
                        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - ctrlW, rowY + (rowH - ctrlH) * 0.5f));
                        SettingControl(item, controlW);
                        ImGui::SetCursorPosY(rowY + rowH);
                        const ImVec2 lp = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddLine(lp, ImVec2(lp.x + ImGui::GetContentRegionAvail().x, lp.y), Rgba(239, 230, 210, 0.05f), 1.0f);
                        ImGui::Dummy(ImVec2(0, 2.0f * S));
                        ImGui::PopID();
                    }
                }
            }
            EndCard();

            // Apply / revert.
            int changes = 0;
            for (const auto &sec : kSections) {
                changes += SectionChanges(sec);
            }
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + listW + 24.0f * S, top + h + 12.0f * S));
            const std::string summary = changes == 0 ? "No unsaved changes" : std::to_string(changes) + (changes == 1 ? " unsaved change" : " unsaved changes");
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14.0f * S);
            Label(nullptr, 16.0f, changes ? kBrass : kInk4, summary.c_str());
            const float bw = 150.0f * S;
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + avail.x - bw * 2 - 12.0f * S, top + h + 12.0f * S));
            if (Button("revert", "Revert", ImVec2(bw, 46.0f * S), Tone::Secondary, 0, changes > 0)) {
                g_mpStage = g_mpLive;
                std::snprintf(g_nickname, sizeof(g_nickname), "%s", g_mpLive.nickname.c_str());
                g_gameStage = g_gameLive;
            }
            ImGui::SameLine(0, 12.0f * S);
            if (Button("apply", "Apply", ImVec2(bw, 46.0f * S), Tone::Primary, 0, changes > 0)) {
                ApplySettings();
            }
        }

        // =============================================================================================================
        // the title screen (MTA-style: logo, a column of menu items, news, version)
        // =============================================================================================================
        void Backdrop(ImDrawList *dl, ImVec2 ds) {
            if (!g_inSession) {
                dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, Rgba(27, 18, 12), Rgba(16, 11, 8), Rgba(5, 4, 3), Rgba(11, 8, 6));
                RadialGlow(dl, ImVec2(ds.x * 0.78f, ds.y * 0.70f), ds.y * 0.95f, 179, 38, 30, 0.22f);
                RadialGlow(dl, ImVec2(ds.x * 0.10f, ds.y * 0.05f), ds.y * 0.70f, 201, 161, 90, 0.07f);
                const float v = ds.y * 0.30f;
                dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ds.x, v), Rgba(0, 0, 0, 0.40f), Rgba(0, 0, 0, 0.40f), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0));
                dl->AddRectFilledMultiColor(ImVec2(0, ds.y - v), ds, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.60f), Rgba(0, 0, 0, 0.60f));
            }
            else {
                dl->AddRectFilled(ImVec2(0, 0), ds, Rgba(5, 4, 3, 0.50f));
            }
            // The menu column sits on a soft dark band.
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ds.x * 0.45f, ds.y), Rgba(4, 3, 2, 0.80f), Rgba(4, 3, 2, 0.0f), Rgba(4, 3, 2, 0.0f),
                                        Rgba(4, 3, 2, 0.80f));
        }

        void Logo(ImDrawList *dl, ImVec2 pos) {
            // Type only: RED DEAD in the heading face, MULTIPLAYER spaced out in red under it, flush left.
            PutSpaced(dl, g_title, 84.0f, ImVec2(pos.x + 2.0f * S, pos.y + 3.0f * S), Rgba(0, 0, 0, 0.6f), "RED DEAD", 1.0f); // shadow
            const float w       = PutSpaced(dl, g_title, 84.0f, pos, kInk, "RED DEAD", 1.0f);
            const float y       = pos.y + 92.0f * S;
            const float mw      = PutSpaced(dl, nullptr, 21.0f, ImVec2(0, 0), kRedHi, "MULTIPLAYER", 0.0f, true);
            const float spacing = std::max(0.0f, (w - mw) / 10.0f / S); // MULTIPLAYER spans the width of RED DEAD
            PutSpaced(dl, nullptr, 21.0f, ImVec2(pos.x + 3.0f * S, y), kRedHi, "MULTIPLAYER", spacing);
        }

        // One title-screen item: big type, slides right and brightens on hover, a red mark in front.
        bool MenuItem(const char *text, ImVec2 pos, bool enabled = true) {
            static std::unordered_map<std::string, float> anim;
            float &t         = anim[text];
            const float size = 36.0f;
            const ImVec2 ts  = Measure(g_title, size, text);
            ImGui::SetCursorScreenPos(ImVec2(pos.x - 24.0f * S, pos.y));
            ImGui::PushID(text);
            const bool pressed = ImGui::InvisibleButton("##mi", ImVec2(ts.x + 64.0f * S, ts.y + 6.0f * S)) && enabled;
            const bool hovered = enabled && ImGui::IsItemHovered();
            ImGui::PopID();
            const float dt = ImGui::GetIO().DeltaTime;
            t              = std::clamp(t + (hovered ? dt : -dt) * 8.0f, 0.0f, 1.0f);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float x  = pos.x + 16.0f * S * t;
            if (t > 0.0f) {
                dl->AddRectFilled(ImVec2(pos.x - 14.0f * S, pos.y + ts.y * 0.22f), ImVec2(pos.x - 10.0f * S, pos.y + ts.y * 0.80f), Rgba(214, 56, 44, t));
            }
            const ImU32 col = enabled ? IM_COL32(static_cast<int>(201 + (255 - 201) * t), static_cast<int>(189 + (246 - 189) * t),
                                                 static_cast<int>(163 + (232 - 163) * t), 255)
                                      : kInk4;
            Put(dl, g_title, size, ImVec2(x + 2.0f * S, pos.y + 2.0f * S), Rgba(0, 0, 0, 0.55f), text);
            Put(dl, g_title, size, ImVec2(x, pos.y), col, text);
            return pressed;
        }

        // ---- windows over the title screen ----
        enum class Win { Browser, Direct, Settings, About };
        std::vector<Win> g_open; // opening order: Esc closes the last
        bool IsOpen(Win w) {
            return std::find(g_open.begin(), g_open.end(), w) != g_open.end();
        }
        void Close(Win w) {
            g_open.erase(std::remove(g_open.begin(), g_open.end(), w), g_open.end());
        }
        Win g_focusWin   = Win::Browser;
        bool g_focusWant = false;
        void Open(Win w) {
            if (!IsOpen(w)) {
                g_open.push_back(w);
                if (w == Win::Browser && !g_listAsked[g_list]) {
                    g_listAsked[g_list] = true;
                    M::Refresh(kLists[g_list]);
                }
                if (w == Win::Settings) {
                    g_mpLive = g_mpStage = M::LoadMp();
                    std::snprintf(g_nickname, sizeof(g_nickname), "%s", g_mpStage.nickname.c_str());
                    g_gameAvailable = M::ReadGame(g_gameLive);
                    g_gameStage     = g_gameLive;
                }
            }
            g_focusWin  = w;
            g_focusWant = true;
        }

        void NewsPanel(ImVec2 ds) {
            const auto &log = M::Changelog();
            if (log.empty()) {
                return;
            }
            const float w = 460.0f * S, h = 330.0f * S;
            ImGui::SetCursorScreenPos(ImVec2(ds.x - w - 64.0f * S, ds.y - h - 92.0f * S));
            if (BeginCard("##news", ImVec2(w, h), true)) {
                CardTitle("Latest news");
                bool first = true;
                for (size_t i = 0; i < log.size() && i < 3; ++i) {
                    if (!first) {
                        Gap(10);
                    }
                    first = false;
                    Label(g_title, 20.0f, kInk, log[i].heading.c_str());
                    for (const auto &line : log[i].lines) {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 4.0f * S, p.y + 10.0f * S), 2.5f * S, kRed, 8);
                        ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f * S, p.y));
                        Label(nullptr, 15.0f, kInk2, line.c_str(), ImGui::GetContentRegionAvail().x - 16.0f * S);
                    }
                }
            }
            EndCard();
        }

        void TitleScreen(ImVec2 ds) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            Backdrop(dl, ds);
            const float x = 110.0f * S;
            Logo(dl, ImVec2(x, 96.0f * S));

            float y          = ds.y * 0.40f;
            const float step = 62.0f * S;
            auto item        = [&](const char *text) {
                const bool pressed = MenuItem(text, ImVec2(x, y));
                y += step;
                return pressed;
            };
            if (g_inSession) {
                if (item("RESUME")) Frontend::CloseInGameMenu();
                if (item("SETTINGS")) Open(Win::Settings);
                if (item("DISCONNECT")) {
                    M::Disconnect();
                    M::Notify(false, "You left the server.");
                }
                y += step * 0.35f;
                if (item("QUIT")) M::Quit();
            }
            else {
                if (item("QUICK CONNECT")) Open(Win::Direct);
                if (item("BROWSE SERVERS")) Open(Win::Browser);
                if (item("SETTINGS")) Open(Win::Settings);
                if (item("ABOUT")) Open(Win::About);
                y += step * 0.35f;
                if (item("QUIT")) M::Quit();
            }

            if (!g_inSession) {
                NewsPanel(ds);
            }
            else {
                const int ping       = M::PingMs();
                const std::string at = "Connected to " + (M::PendingAddress().empty() ? std::string("a server") : M::PendingAddress()) +
                                       (ping >= 0 ? "   ·   " + std::to_string(ping) + " ms" : "");
                Put(dl, nullptr, 16.0f, ImVec2(x, y + 12.0f * S), kInk3, at.c_str());
            }
            const M::MpSettings mp  = M::LoadMp();
            const std::string who   = "Playing as " + mp.nickname;
            Put(dl, nullptr, 16.0f, ImVec2(x, ds.y - 64.0f * S), kInk2, who.c_str());
            const std::string build = "Red Dead Multiplayer " + M::Version();
            const ImVec2 bs         = Measure(nullptr, 15.0f, build.c_str());
            Put(dl, nullptr, 15.0f, ImVec2(ds.x - bs.x - 64.0f * S, ds.y - 52.0f * S), kInk4, build.c_str());
            ImGui::SetCursorScreenPos(ImVec2(0, 0));
            ImGui::Dummy(ImVec2(0, 0));
        }

        // A movable window in the menu's look: Palatino title, a red rule under the title bar.
        bool BeginWindow(Win which, const char *title, ImVec2 size, bool resizable) {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
            ImGui::SetNextWindowPos(ImVec2(std::max(ds.x * 0.5f, 560.0f * S + size.x * 0.5f), ds.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (g_focusWant && g_focusWin == which) {
                ImGui::SetNextWindowFocus();
                g_focusWant = false;
            }
            bool open = true;
            ImGui::PushFont(Or(g_title), 20.0f);
            const float titleH = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
            const bool shown =
                ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | (resizable ? 0 : ImGuiWindowFlags_NoResize));
            ImGui::PopFont();
            if (!open) {
                Close(which);
            }
            if (shown) {
                const ImVec2 p = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + titleH - 2.0f * S), ImVec2(p.x + ImGui::GetWindowWidth(), p.y + titleH), kRed);
            }
            return shown;
        }

        void AboutWindow() {
            if (BeginWindow(Win::About, "About", ImVec2(560.0f * S, 0), false)) {
                Label(g_title, 30.0f, kInk, "Red Dead Multiplayer");
                Label(nullptr, 15.0f, kInk3, ("Version " + M::Version()).c_str());
                Gap(8);
                Label(nullptr, 16.0f, kInk2,
                      "A multiplayer modification for Red Dead Redemption on PC, built on the MafiaHub Framework: servers with "
                      "JavaScript resources, synced players, horses and blips.",
                      500.0f * S);
                Gap(8);
                Label(nullptr, 14.0f, kInk4,
                      "Unofficial fan project, not affiliated with or endorsed by Rockstar Games or Take-Two Interactive. Red Dead "
                      "Redemption is a trademark of its owners.",
                      500.0f * S);
                Gap(6);
                if (Button("close", "Close", ImVec2(140.0f * S, 40.0f * S))) {
                    Close(Win::About);
                }
            }
            ImGui::End();
        }

        void DirectWindow() {
            if (BeginWindow(Win::Direct, "Quick Connect", ImVec2(520.0f * S, 0), false)) {
                Label(nullptr, 15.0f, kInk2, "Address");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::IsWindowAppearing()) {
                    ImGui::SetKeyboardFocusHere();
                }
                bool go = ImGui::InputTextWithHint("##addr", "127.0.0.1:27015", g_address, sizeof(g_address), ImGuiInputTextFlags_EnterReturnsTrue);
                Label(nullptr, 15.0f, kInk2, "Password (if the server has one)");
                ImGui::SetNextItemWidth(-1);
                go |= ImGui::InputText("##pw", g_directPassword, sizeof(g_directPassword), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
                Label(nullptr, 13.0f, kInk4, "Port 27015 when left out.");
                Gap(6);
                const float bw = (ImGui::GetContentRegionAvail().x - 10.0f * S) * 0.5f;
                if (Button("back", "Back", ImVec2(bw, 42.0f * S))) {
                    Close(Win::Direct);
                }
                ImGui::SameLine(0, 10.0f * S);
                go |= Button("connect", "Connect", ImVec2(bw, 42.0f * S), Tone::Primary, 0xE768);
                if (go) {
                    std::string host;
                    int port = 0;
                    if (M::ParseAddress(g_address, host, port)) {
                        M::Connect(host, port, g_directPassword);
                    }
                    else {
                        M::Notify(true, "That address cannot be read. Use host or host:port.");
                    }
                }
            }
            ImGui::End();
        }

        // =============================================================================================================
        // dialogs and notices
        // =============================================================================================================
        bool BeginDialog(const char *id, float width) {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(width, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f * S, 26.0f * S));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kLine2));
            const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            if (open) {
                // A red rule along the top edge.
                const ImVec2 p = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y + 3.0f * S), kRed);
            }
            return open;
        }

        void Dialogs() {
            // Connecting.
            const bool connecting = M::ConnectionPhase() == M::Phase::Connecting;
            if (connecting && !ImGui::IsPopupOpen("##connecting")) {
                ImGui::OpenPopup("##connecting");
            }
            if (BeginDialog("##connecting", 520.0f * S)) {
                if (!connecting) {
                    ImGui::CloseCurrentPopup();
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                Spinner(ImGui::GetWindowDrawList(), ImVec2(p.x + 16.0f * S, p.y + 22.0f * S), 14.0f * S, kRedHi);
                ImGui::SetCursorScreenPos(ImVec2(p.x + 50.0f * S, p.y));
                ImGui::BeginGroup();
                Label(g_title, 26.0f, kInk, "Joining");
                Label(nullptr, 15.0f, kInk3, (M::ConnectingStep() + "  ·  " + M::PendingAddress()).c_str());
                ImGui::EndGroup();
                Gap(18);
                if (Button("cancel", "Cancel", ImVec2(ImGui::GetContentRegionAvail().x, 44.0f * S), Tone::Secondary)) {
                    M::Disconnect();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Password.
            if (g_askPassword && !ImGui::IsPopupOpen("##password")) {
                ImGui::OpenPopup("##password");
            }
            if (BeginDialog("##password", 480.0f * S)) {
                Label(g_title, 26.0f, kInk, "Password required");
                Label(nullptr, 15.0f, kInk3, (g_joinHost + ":" + std::to_string(g_joinPort)).c_str());
                Gap(12);
                if (ImGui::IsWindowAppearing()) {
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(-1);
                bool go = ImGui::InputText("##jpw", g_joinPassword, sizeof(g_joinPassword), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
                Gap(12);
                const float bw = (ImGui::GetContentRegionAvail().x - 12.0f * S) * 0.5f;
                const bool cancel = Button("cancel", "Cancel", ImVec2(bw, 44.0f * S), Tone::Secondary) || !g_askPassword;
                ImGui::SameLine(0, 12.0f * S);
                go |= Button("join", "Join", ImVec2(bw, 44.0f * S), Tone::Primary, 0xE768);
                if (go) {
                    g_askPassword = false;
                    ImGui::CloseCurrentPopup();
                    M::Connect(g_joinHost, g_joinPort, g_joinPassword);
                }
                else if (cancel) {
                    g_askPassword = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Keep display settings.
            if (g_keepAsk && !ImGui::IsPopupOpen("##keep")) {
                ImGui::OpenPopup("##keep");
            }
            if (BeginDialog("##keep", 500.0f * S)) {
                const int left = g_keepAsk ? static_cast<int>((g_keepUntil > NowMs() ? g_keepUntil - NowMs() : 0) / 1000) + 1 : 0;
                Label(g_title, 26.0f, kInk, "Keep these display settings?");
                Label(nullptr, 15.0f, kInk3, ("They go back in " + std::to_string(left) + (left == 1 ? " second." : " seconds.")).c_str());
                Gap(16);
                const float bw = (ImGui::GetContentRegionAvail().x - 12.0f * S) * 0.5f;
                const bool revert = Button("revert", "Revert", ImVec2(bw, 44.0f * S), Tone::Secondary) || (g_keepAsk && NowMs() >= g_keepUntil);
                ImGui::SameLine(0, 12.0f * S);
                const bool keep = Button("keep", "Keep", ImVec2(bw, 44.0f * S), Tone::Primary);
                if (!g_keepAsk) {
                    ImGui::CloseCurrentPopup();
                }
                else if (keep) {
                    g_keepAsk = false;
                    M::Notify(false, "Settings saved.");
                    ImGui::CloseCurrentPopup();
                }
                else if (revert) {
                    RevertDisplay();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void Toast(ImVec2 ds) {
            const M::Notice &n = M::LastNotice();
            if (n.text.empty() || NowMs() - n.atMs > 4500) {
                return;
            }
            const float age   = static_cast<float>(NowMs() - n.atMs);
            const float alpha = std::clamp(std::min(age / 150.0f, (4500.0f - age) / 400.0f), 0.0f, 1.0f);
            ImDrawList *dl    = ImGui::GetForegroundDrawList();
            const ImVec2 ts   = Measure(nullptr, 17.0f, n.text.c_str());
            const ImVec2 size = ImVec2(ts.x + 64.0f * S, 52.0f * S);
            const ImVec2 pos  = ImVec2((ds.x - size.x) * 0.5f + 210.0f * S, ds.y - size.y - 48.0f * S);
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(22, 17, 13, 0.97f * alpha), 6.0f * S);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(239, 230, 210, 0.16f * alpha), 6.0f * S);
            dl->AddRectFilled(pos, ImVec2(pos.x + 4.0f * S, pos.y + size.y), n.error ? Rgba(214, 56, 44, alpha) : Rgba(201, 161, 90, alpha), 6.0f * S,
                              ImDrawFlags_RoundCornersLeft);
            Put(dl, nullptr, 17.0f, ImVec2(pos.x + 32.0f * S, pos.y + (size.y - ts.y) * 0.5f), Rgba(239, 230, 210, alpha), n.text.c_str());
        }
    } // namespace

    void Attach(FwClient *client) {
        g_client = client;
    }

    void Opened(bool inSession) {
        g_inSession = inSession;
        g_focusNext = true;
        if (inSession) {
            Close(Win::Browser);
            Close(Win::Direct);
            Close(Win::About);
        }
    }

    bool Back() {
        if (g_keepAsk) {
            RevertDisplay();
            return true;
        }
        if (g_askPassword) {
            g_askPassword = false;
            return true;
        }
        if (M::ConnectionPhase() == M::Phase::Connecting) {
            M::Disconnect();
            return true;
        }
        if (!g_open.empty()) {
            g_open.pop_back();
            return true;
        }
        return false;
    }

    void Draw(bool inSession) {
        if (inSession != g_inSession) {
            Opened(inSession);
        }
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (ds.x <= 0 || ds.y <= 0) {
            return;
        }
        S = std::clamp(ds.y / 1080.0f, 0.7f, 3.0f);
        LookUpFonts();
        ApplyTheme();

        // The title screen: a full-screen window under the others.
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ds);
        if (g_focusNext) {
            ImGui::SetNextWindowFocus();
            g_focusNext = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
        const bool open = ImGui::Begin("##rdmp_menu", nullptr, flags);
        ImGui::PopStyleVar(2);
        if (open) {
            TitleScreen(ds);
            Dialogs();
        }
        ImGui::End();

        // The windows, in the order they were opened.
        const std::vector<Win> order = g_open;
        for (Win w : order) {
            switch (w) {
            case Win::Browser:
                if (BeginWindow(Win::Browser, "Server Browser", ImVec2(1180.0f * S, 720.0f * S), true)) {
                    ServersPage(ImGui::GetContentRegionAvail());
                }
                ImGui::End();
                break;
            case Win::Direct: DirectWindow(); break;
            case Win::Settings:
                if (BeginWindow(Win::Settings, "Settings", ImVec2(1180.0f * S, 760.0f * S), true)) {
                    SettingsPage(ImGui::GetContentRegionAvail());
                }
                ImGui::End();
                break;
            case Win::About: AboutWindow(); break;
            }
        }
        Toast(ds);
    }
} // namespace RDMP::MenuUi
