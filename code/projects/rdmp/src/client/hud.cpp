/*
 * Overlay content: script dx* drawing, nametags, chat box. See hud.h.
 */

#include <utils/safe_win32.h>

#include <imgui.h>

#include <integrations/client/ui/nametag_imgui.h>

#include "client/fw_client.h"
#include "client/hud.h"
#include "client/overlay.h"
#include "shared/fw_api.h"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace RDMP::Hud {
    namespace {
        namespace Tags = Framework::Integrations::Client::UI::Nametags;

        FwClient *g_client           = nullptr;
        const rdmp::fw::Host *g_host = nullptr;

        struct Cmd {
            enum class Kind : uint8_t { Text, Rect, Line, Circle } kind;
            float a = 0, b = 0, c = 0, d = 0; // x,y,(w,h | x2,y2 | radius)
            float e = 0;                      // scale / rounding / width
            uint32_t color = 0xFFFFFFFF;
            Align align    = Align::Left;
            bool flag      = false; // text: shadow, circle: filled
            std::string text;
        };
        std::vector<Cmd> g_cmds;

        bool g_tagsEnabled = true;
        float g_tagDistance = 60.0f;
        std::unordered_map<uint64_t, int> g_tagOverride;

        ImU32 FromARGB(uint32_t argb) {
            return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
        }

        void DrawScriptCommands(ImDrawList *list) {
            ImFont *font     = ImGui::GetFont();
            const float base = ImGui::GetFontSize();
            for (const auto &c : g_cmds) {
                switch (c.kind) {
                case Cmd::Kind::Text: {
                    const float size = base * (c.e > 0 ? c.e : 1.0f);
                    float x          = c.a;
                    if (c.align != Align::Left) {
                        const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, c.text.c_str()).x;
                        x -= c.align == Align::Center ? w * 0.5f : w;
                    }
                    if (c.flag) {
                        const float o = std::max(1.0f, size / 14.0f);
                        list->AddText(font, size, ImVec2(x + o, c.b + o), IM_COL32(0, 0, 0, (c.color >> 24) & 0xFF), c.text.c_str());
                    }
                    list->AddText(font, size, ImVec2(x, c.b), FromARGB(c.color), c.text.c_str());
                    break;
                }
                case Cmd::Kind::Rect: list->AddRectFilled(ImVec2(c.a, c.b), ImVec2(c.a + c.c, c.b + c.d), FromARGB(c.color), c.e); break;
                case Cmd::Kind::Line: list->AddLine(ImVec2(c.a, c.b), ImVec2(c.c, c.d), FromARGB(c.color), c.e > 0 ? c.e : 1.0f); break;
                case Cmd::Kind::Circle:
                    if (c.flag) {
                        list->AddCircleFilled(ImVec2(c.a, c.b), c.c, FromARGB(c.color));
                    }
                    else {
                        list->AddCircle(ImVec2(c.a, c.b), c.c, FromARGB(c.color), 0, c.e > 0 ? c.e : 1.0f);
                    }
                    break;
                }
            }
            g_cmds.clear();
        }

        void DrawNametags(ImDrawList *list) {
            if (!g_tagsEnabled || !g_client) {
                return;
            }
            float cx, cy, cz;
            if (!CameraPosition(cx, cy, cz)) {
                return;
            }
            const ImVec2 viewport = ImGui::GetIO().DisplaySize;
            Tags::Layout layout;
            layout.drawDistance = g_tagDistance;
            const Tags::Appearance appearance;

            const auto draw = [&](Shared::Player *p, bool self) {
                const int over = NametagOverride(static_cast<uint64_t>(p->GetNetworkID()));
                const bool showing = over >= 0 ? over == 1 : (p->nametagShowing != 0 && !self);
                if (!showing) {
                    return;
                }
                // Where the game draws them: the puppet for a remote player, the live position for us.
                float at[3] = {p->position.x, p->position.y, p->position.z};
                if (self) {
                    float heading, health;
                    g_host->get_local_player(at, &heading, &health, nullptr);
                }
                else if (g_host->get_remote_position && !g_host->get_remote_position(static_cast<uint64_t>(p->GetNetworkID()), at)) {
                    return; // no puppet (streamed out): no tag
                }
                const float hx = at[0], hy = at[1] + 1.9f, hz = at[2]; // just above the head
                const float dx = hx - cx, dy = hy - cy, dz = hz - cz;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > layout.drawDistance) {
                    return;
                }
                float sx, sy;
                if (!WorldToScreen(hx, hy, hz, sx, sy)) {
                    return;
                }
                const std::string &label = p->nametagText.empty() ? p->nickname : p->nametagText;
                Tags::Resolved tag;
                tag.id            = static_cast<uint64_t>(p->GetNetworkID());
                tag.distance      = distance;
                tag.distanceAlpha = Tags::DistanceAlpha(distance, layout);
                tag.scale         = Tags::DistanceScale(distance, layout);
                tag.components    = static_cast<uint8_t>(Tags::Component::Name) | static_cast<uint8_t>(Tags::Component::Health);
                tag.color         = p->nametagColor ? p->nametagColor : 0xFFFFFFFF;
                tag.healthPercent = static_cast<float>(p->localHealth);
                tag.label         = label.c_str();
                Tags::DrawResolvedNametag(list, tag, ImVec2(sx, sy), 1.0f, appearance, viewport);
            };
            g_client->ForEachRemote([&](Shared::Player *p) {
                draw(p, false);
            });
            if (auto *me = g_client->Owned()) {
                draw(me, true);
            }
        }
    } // namespace

    void Attach(FwClient *client, const rdmp::fw::Host *host) {
        g_client = client;
        g_host   = host;
    }

    void Draw() {
        static int frames = 0;
        if (frames++ == 0) {
            Log()->info("hud: first widget pass (display {}x{}, chat session {}, visible {})", ImGui::GetIO().DisplaySize.x,
                        ImGui::GetIO().DisplaySize.y, g_client && g_client->GetChatBox().IsSessionActive(), g_client && g_client->GetChatBox().IsVisible());
        }
        ImDrawList *list = ImGui::GetBackgroundDrawList();
        DrawNametags(list);
        DrawScriptCommands(list);
        if (g_client) {
            g_client->GetChatBox().Render();
        }
    }

    void QueueText(const std::string &text, float x, float y, uint32_t color, float scale, Align align, bool shadow) {
        Cmd c {Cmd::Kind::Text, x, y};
        c.e = scale, c.color = color, c.align = align, c.flag = shadow, c.text = text;
        g_cmds.push_back(std::move(c));
    }
    void QueueRect(float x, float y, float w, float h, uint32_t color, float rounding) {
        Cmd c {Cmd::Kind::Rect, x, y, w, h, rounding};
        c.color = color;
        g_cmds.push_back(c);
    }
    void QueueLine(float x1, float y1, float x2, float y2, uint32_t color, float width) {
        Cmd c {Cmd::Kind::Line, x1, y1, x2, y2, width};
        c.color = color;
        g_cmds.push_back(c);
    }
    void QueueCircle(float x, float y, float radius, uint32_t color, bool filled, float width) {
        Cmd c {Cmd::Kind::Circle, x, y, radius, 0, width};
        c.color = color, c.flag = filled;
        g_cmds.push_back(c);
    }

    float TextWidth(const std::string &text, float scale) {
        if (!Overlay::Ready() || !ImGui::GetCurrentContext() || !ImGui::GetFont()) {
            return 0.0f;
        }
        return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * (scale > 0 ? scale : 1.0f), FLT_MAX, 0.0f, text.c_str()).x;
    }
    float FontHeight(float scale) {
        if (!Overlay::Ready() || !ImGui::GetCurrentContext()) {
            return 0.0f;
        }
        return ImGui::GetFontSize() * (scale > 0 ? scale : 1.0f);
    }

    bool CameraPosition(float &x, float &y, float &z) {
        rdmp::fw::Camera cam {};
        if (!g_host || !g_host->get_camera || !g_host->get_camera(&cam)) {
            return false;
        }
        x = cam.pos[0], y = cam.pos[1], z = cam.pos[2];
        return true;
    }

    bool WorldToScreen(float x, float y, float z, float &sx, float &sy) {
        rdmp::fw::Camera cam {};
        float width, height;
        Overlay::ScreenSize(width, height);
        if (width <= 0 || height <= 0 || !g_host || !g_host->get_camera || !g_host->get_camera(&cam)) {
            return false;
        }
        const float d[3]    = {x - cam.pos[0], y - cam.pos[1], z - cam.pos[2]};
        const float depth   = -(d[0] * cam.back[0] + d[1] * cam.back[1] + d[2] * cam.back[2]);
        if (depth < 0.1f) {
            return false;
        }
        const float right   = d[0] * cam.right[0] + d[1] * cam.right[1] + d[2] * cam.right[2];
        const float up      = d[0] * cam.up[0] + d[1] * cam.up[1] + d[2] * cam.up[2];
        const float tanHalf = std::tan(cam.fovY * 0.5f * 3.14159265f / 180.0f);
        const float aspect  = width / height;
        const float ndcX    = right / (depth * tanHalf * aspect);
        const float ndcY    = up / (depth * tanHalf);
        sx = (ndcX * 0.5f + 0.5f) * width;
        sy = (0.5f - ndcY * 0.5f) * height;
        return true;
    }

    void SetNametagsEnabled(bool enabled) {
        g_tagsEnabled = enabled;
    }
    bool NametagsEnabled() {
        return g_tagsEnabled;
    }
    void SetNametagOverride(uint64_t playerId, int showing) {
        if (showing < 0) {
            g_tagOverride.erase(playerId);
        }
        else {
            g_tagOverride[playerId] = showing ? 1 : 0;
        }
    }
    int NametagOverride(uint64_t playerId) {
        const auto it = g_tagOverride.find(playerId);
        return it == g_tagOverride.end() ? -1 : it->second;
    }
    void SetNametagDrawDistance(float meters) {
        g_tagDistance = meters > 1.0f ? meters : 1.0f;
    }

    void ChatLine(const std::string &text, uint32_t colorRGBA) {
        if (g_client) {
            g_client->GetChatBox().AddMessage("", text, colorRGBA);
        }
    }
    void SetChatVisible(bool visible) {
        if (g_client) {
            g_client->GetChatBox().SetVisible(visible);
        }
    }
    bool ChatVisible() {
        return g_client && g_client->GetChatBox().IsVisible();
    }
    bool ChatInputActive() {
        return g_client && g_client->GetChatBox().IsInputActive();
    }
} // namespace RDMP::Hud
