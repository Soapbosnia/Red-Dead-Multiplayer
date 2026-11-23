/*
 * Overlay content: script dx* drawing, nametags, chat box. See hud.h.
 */

#include <utils/safe_win32.h>

#include <imgui.h>

#include "client/api.h"
#include "client/fw_client.h"
#include "client/hud.h"
#include "client/overlay.h"
#include "shared/fw_api.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace RDMP::Hud {
    namespace {
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

        // ---- look ----
        float Scale() {
            const float h = ImGui::GetIO().DisplaySize.y;
            return std::clamp(h / 1080.0f, 0.7f, 3.0f);
        }

        ImFont *Named(const char *name) {
            auto *imgui = g_client ? g_client->GetImGUI() : nullptr;
            ImFont *font = imgui ? imgui->GetFont(name) : nullptr;
            return font ? font : ImGui::GetFont();
        }

        ImU32 WithAlpha(ImU32 c, float alpha) {
            const int a = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
            return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
        }

        // 0xRRGGBBAA (alpha 0 reads as opaque, so a bare 0xRRGGBB shows), as the Framework's chat colours are packed.
        ImU32 FromRGBA(uint32_t c, ImU32 fallback) {
            if (c == 0) {
                return fallback;
            }
            const uint32_t a = c & 0xFF;
            return IM_COL32((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, a ? a : 255);
        }

        // Text with a soft dark outline, readable over any part of the world.
        void OutlinedText(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, const char *end = nullptr) {
            const float o       = std::max(1.0f, size / 18.0f);
            const ImU32 outline = WithAlpha(IM_COL32(0, 0, 0, 225), ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
            dl->AddText(font, size, ImVec2(pos.x + o, pos.y + o), outline, text, end);
            dl->AddText(font, size, ImVec2(pos.x - o * 0.5f, pos.y + o), outline, text, end);
            dl->AddText(font, size, ImVec2(pos.x + o, pos.y - o * 0.5f), outline, text, end);
            dl->AddText(font, size, pos, col, text, end);
        }

        const ImU32 kChatText   = IM_COL32(239, 230, 210, 255); // ivory
        const ImU32 kChatName   = IM_COL32(222, 178, 98, 255);  // brass
        const ImU32 kChatNotice = IM_COL32(236, 206, 132, 255); // warm yellow
        const ImU32 kRed        = IM_COL32(182, 22, 18, 255);

        // ---- chat ----
        struct ChatEntry {
            std::string author; // empty = a notice
            std::string text;
            uint32_t color = 0;
            double time    = 0;
        };
        std::deque<ChatEntry> g_chat;
        char g_input[256] = "";
        std::deque<std::string> g_history;
        int g_historyPos   = -1;
        bool g_wasTyping   = false;
        bool g_wasSession  = false;
        constexpr double kChatHold = 14.0, kChatFade = 2.0;
        constexpr size_t kChatKeep = 100;

        void AddChat(const std::string &author, const std::string &text, uint32_t color) {
            if (text.empty() || !g_client || !g_client->GetChatBox().IsVisible()) {
                return;
            }
            g_chat.push_back({author, text, color, ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0});
            while (g_chat.size() > kChatKeep) {
                g_chat.pop_front();
            }
        }

        int HistoryCallback(ImGuiInputTextCallbackData *data) {
            if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty()) {
                return 0;
            }
            const int before = g_historyPos;
            if (data->EventKey == ImGuiKey_UpArrow) {
                g_historyPos = g_historyPos == -1 ? static_cast<int>(g_history.size()) - 1 : std::max(0, g_historyPos - 1);
            }
            else if (data->EventKey == ImGuiKey_DownArrow && g_historyPos != -1) {
                g_historyPos = g_historyPos + 1 >= static_cast<int>(g_history.size()) ? -1 : g_historyPos + 1;
            }
            if (before != g_historyPos) {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, g_historyPos >= 0 ? g_history[g_historyPos].c_str() : "");
            }
            return 0;
        }

        void SendTyped() {
            std::string text = g_input;
            const size_t a   = text.find_first_not_of(" \t");
            if (a == std::string::npos) {
                return;
            }
            text = text.substr(a, text.find_last_not_of(" \t") - a + 1);
            if (g_history.empty() || g_history.back() != text) {
                g_history.push_back(text);
                if (g_history.size() > 50) {
                    g_history.pop_front();
                }
            }
            // As the Framework's own box does: "chatSend" first (a handler can block it), then to the server.
            if (ClientApi::EmitChatSend(text)) {
                g_client->SendChatMessage(text);
            }
        }

        struct ChatLineLayout {
            const ChatEntry *entry;
            const char *begin;
            const char *end;
            bool first; // carries the author
            float alpha;
        };

        void DrawChat() {
            auto &box = g_client->GetChatBox();
            if (box.IsSessionActive() != g_wasSession) {
                g_wasSession = box.IsSessionActive();
                g_chat.clear(); // a new session starts with an empty box
            }
            if (!box.IsSessionActive() || !box.IsVisible()) {
                return;
            }
            const bool typing = box.IsInputActive();
            const double now  = ImGui::GetTime();
            const float S     = Scale();
            const float size  = 19.0f * S;
            const float lineH = size * 1.3f;
            const float x0 = 30.0f * S, y0 = 28.0f * S, width = 560.0f * S;
            ImFont *body = Named("semibold"), *bold = Named("bold");

            // Lay the lines out (wrapped), newest last; idle shows only recent ones, fading.
            std::vector<ChatLineLayout> lines;
            for (const auto &e : g_chat) {
                float alpha = 1.0f;
                if (!typing) {
                    const double age = now - e.time;
                    if (age > kChatHold + kChatFade) {
                        continue;
                    }
                    alpha = age > kChatHold ? 1.0f - static_cast<float>((age - kChatHold) / kChatFade) : 1.0f;
                }
                const float nameW = e.author.empty() ? 0.0f : bold->CalcTextSizeA(size, FLT_MAX, 0.0f, (e.author + ": ").c_str()).x;
                const char *p     = e.text.c_str();
                const char *end   = p + e.text.size();
                bool first        = true;
                while (p < end) {
                    const float avail = width - (first ? nameW : 0.0f);
                    const char *cut   = body->CalcWordWrapPosition(size, p, end, avail);
                    if (cut == p) {
                        cut = p + 1;
                    }
                    lines.push_back({&e, p, cut, first, alpha});
                    p = cut;
                    while (p < end && *p == ' ') {
                        ++p;
                    }
                    first = false;
                }
            }
            const size_t maxLines = typing ? 14 : 9;
            const size_t from     = lines.size() > maxLines ? lines.size() - maxLines : 0;
            const float logH      = static_cast<float>(lines.size() - from) * lineH;

            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            if (typing) {
                // A dark panel while typing, the red edge of the menu's look.
                const float panelH = std::max(logH, lineH * 4) + lineH + 30.0f * S;
                const ImVec2 p0(x0 - 14.0f * S, y0 - 10.0f * S), p1(x0 + width + 14.0f * S, y0 + panelH);
                dl->AddRectFilled(p0, p1, IM_COL32(10, 7, 5, 196), 6.0f * S);
                dl->AddRect(p0, p1, IM_COL32(239, 230, 210, 30), 6.0f * S);
                dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f * S, p1.y), kRed, 6.0f * S, ImDrawFlags_RoundCornersLeft);
            }
            float y = y0;
            for (size_t i = from; i < lines.size(); ++i) {
                const ChatLineLayout &l = lines[i];
                float x                 = x0;
                if (l.first && !l.entry->author.empty()) {
                    const std::string name = l.entry->author + ": ";
                    OutlinedText(dl, bold, size, ImVec2(x, y), WithAlpha(kChatName, l.alpha), name.c_str());
                    x += bold->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str()).x;
                }
                const ImU32 col = FromRGBA(l.entry->color, l.entry->author.empty() ? kChatNotice : kChatText);
                OutlinedText(dl, body, size, ImVec2(x, y), WithAlpha(col, l.alpha), l.begin, l.end);
                y += lineH;
            }

            // The input line.
            if (typing) {
                if (!g_wasTyping) {
                    g_input[0]   = '\0';
                    g_historyPos = -1;
                }
                const float inputY = y0 + std::max(logH, lineH * 4) + 8.0f * S;
                ImGui::SetNextWindowPos(ImVec2(x0 - 4.0f * S, inputY));
                ImGui::SetNextWindowSize(ImVec2(width + 8.0f * S, lineH + 16.0f * S));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
                if (ImGui::Begin("##rdmp_chat_input", nullptr, flags)) {
                    ImGui::PushFont(bold, 0.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(214, 56, 44, 255)), "Say:");
                    ImGui::PopFont();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(239, 230, 210, 18));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(239, 230, 210, 24));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(239, 230, 210, 28));
                    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(239, 230, 210, 0));
                    if (!g_wasTyping) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    ImGui::SetNextItemWidth(-1);
                    const bool enter = ImGui::InputText("##say", g_input, sizeof(g_input),
                                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, &HistoryCallback);
                    ImGui::PopStyleColor(4);
                    if (enter) {
                        SendTyped();
                        box.CloseInput(false);
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        box.CloseInput(false);
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            }
            g_wasTyping = box.IsInputActive();
        }

        // ---- nametags: name in bold with an outline, a slim red health bar under it ----
        void DrawTag(ImDrawList *dl, float sx, float sy, const std::string &label, uint32_t argb, float health, float distance, float drawDistance) {
            const float S     = Scale();
            const float alpha = std::clamp(1.0f - (distance - drawDistance * 0.6f) / (drawDistance * 0.4f), 0.0f, 1.0f);
            if (alpha <= 0.0f) {
                return;
            }
            const float k    = std::clamp(1.15f - distance / drawDistance * 0.45f, 0.7f, 1.15f);
            const float size = 18.0f * S * k;
            ImFont *bold     = Named("bold");
            const ImVec2 ts  = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
            const float barW = 62.0f * S * k, barH = std::max(3.0f, 5.0f * S * k);
            const float gap  = 4.0f * S * k;
            const bool bar   = health >= 0.0f;
            const float top  = sy - ts.y - (bar ? gap + barH : 0.0f);
            const ImU32 col  = IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
            OutlinedText(dl, bold, size, ImVec2(sx - ts.x * 0.5f, top), WithAlpha(col, alpha), label.c_str());
            if (bar) {
                const ImVec2 b0(sx - barW * 0.5f, top + ts.y + gap), b1(sx + barW * 0.5f, top + ts.y + gap + barH);
                dl->AddRectFilled(ImVec2(b0.x - 1, b0.y - 1), ImVec2(b1.x + 1, b1.y + 1), WithAlpha(IM_COL32(0, 0, 0, 170), alpha), 2.0f);
                const float f = std::clamp(health / 100.0f, 0.0f, 1.0f);
                if (f > 0.0f) {
                    dl->AddRectFilledMultiColor(b0, ImVec2(b0.x + barW * f, b1.y), WithAlpha(IM_COL32(214, 56, 44, 255), alpha),
                                                WithAlpha(IM_COL32(214, 56, 44, 255), alpha), WithAlpha(kRed, alpha), WithAlpha(kRed, alpha));
                }
            }
        }

        void DrawNametags(ImDrawList *list) {
            if (!g_tagsEnabled || !g_client) {
                return;
            }
            float cx, cy, cz;
            if (!CameraPosition(cx, cy, cz)) {
                return;
            }

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
                if (distance > g_tagDistance) {
                    return;
                }
                float sx, sy;
                if (!WorldToScreen(hx, hy, hz, sx, sy)) {
                    return;
                }
                const std::string &label = p->nametagText.empty() ? p->nickname : p->nametagText;
                DrawTag(list, sx, sy, label, p->nametagColor ? p->nametagColor : 0xFFEFE6D2, static_cast<float>(p->localHealth), distance, g_tagDistance);
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
            DrawChat();
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
        AddChat("", text, colorRGBA);
    }

    void ChatMessage(const std::string &author, const std::string &text, uint32_t colorRGBA) {
        AddChat(author, text, colorRGBA);
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
