/*
 * SERVER chat and engine HUD text:
 *   outputChatBox(text[, visibleTo = everyone, r, g, b])   a line in chat boxes (visibleTo: a player, or null)
 *   showGameText(player | null, text[, ms = 4000, style = "subtitle"])
 *       the game's own HUD text on that player's screen (null = everyone). style "subtitle": the bottom-centre line
 *       (one at a time, a new one replaces it); "big": the big centred title.
 *
 * Events (main.cpp): playerChat(player, message) -- return false from a handler to keep the line from being
 * broadcast; playerCommand(player, command, ...args) for lines starting with "/".
 */

#include "server/server.h"

#include <algorithm>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;

        void OutputChatBoxFn(const Args &args) {
            if (!Has(args, 0)) {
                Throw(args.GetIsolate(), "outputChatBox(text[, visibleTo, r, g, b])");
                return;
            }
            const std::string text = Str(args, 0);
            MafiaNet::PeerGuid to  = MafiaNet::UNASSIGNED_PEER_GUID;
            if (Has(args, 1)) {
                to = PlayerGuid(ElementId(args, 1));
                if (to == MafiaNet::UNASSIGNED_PEER_GUID) {
                    Throw(args.GetIsolate(), "outputChatBox: visibleTo is not a connected player");
                    return;
                }
            }
            const bool coloured = Has(args, 2) || Has(args, 3) || Has(args, 4);
            const uint32_t rgba = coloured ? (static_cast<uint32_t>(std::clamp(Num(args, 2, 255), 0.0, 255.0)) << 24 |
                                              static_cast<uint32_t>(std::clamp(Num(args, 3, 255), 0.0, 255.0)) << 16 |
                                              static_cast<uint32_t>(std::clamp(Num(args, 4, 255), 0.0, 255.0)) << 8 | 0xFFu)
                                           : 0u;
            SendChat(to, text, {}, rgba);
            args.GetReturnValue().Set(true);
        }

        void ShowGameTextFn(const Args &args) {
            const char *usage = "showGameText(player | null, text[, ms, \"subtitle\" | \"big\"])";
            if (args.Length() < 2) {
                Throw(args.GetIsolate(), usage);
                return;
            }
            const std::string text  = Str(args, 1);
            const float ms          = Float(args, 2, 4000.0f);
            const std::string style = Str(args, 3, "subtitle");
            const auto kind         = style == "big" ? rdmp::fw::TextStyle::Big : rdmp::fw::TextStyle::Subtitle;
            std::vector<uint64_t> targets;
            if (Has(args, 0)) {
                const uint64_t id = ElementId(args, 0);
                if (!id || !GetWorld().Is<Player>(id)) {
                    Throw(args.GetIsolate(), usage);
                    return;
                }
                targets.push_back(id);
            }
            else {
                targets = GetWorld().IdsOf<Player>();
            }
            for (const uint64_t id : targets) {
                QueueAction(id, ActionKind::HudText, static_cast<float>(kind), ms, 0, 0, text);
            }
            args.GetReturnValue().Set(true);
        }
    } // namespace

    void RegisterHudApi(const ScriptApi::Publisher &publish) {
        publish("outputChatBox", &OutputChatBoxFn);
        publish("showGameText", &ShowGameTextFn);
    }
} // namespace RDMP::Server
