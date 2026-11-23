/*
 * RDMP framework client core (MafiaHub Framework v44), shared by rdmp_bot.exe and rdmp_client.dll (in-game).
 * A headless Integrations::Client::Instance: no renderer, CEF view or Discord presence.
 */

#pragma once

#include <utils/safe_win32.h>

#include <integrations/client/instance.h>
#include <logging/logger.h>
#include <networking/network_client.h>
#include <networking/replication/client_entities.h>
#include <networking/replication/entity_registry.h>
#include <networking/replication/replication_manager.h>

#if __has_include(<imgui.h>)
#include <imgui.h>
#else
// imgui comes in through Framework::Client; if its headers are not on our include path, the two functions we
// need are declared here with the signatures imgui.h gives them (the definitions link from the Framework).
struct ImGuiContext;
struct ImFontAtlas;
namespace ImGui {
    ImGuiContext *CreateContext(ImFontAtlas *shared_font_atlas);
    ImGuiContext *GetCurrentContext();
} // namespace ImGui
#endif

#include "shared/blip.h"
#include "shared/player.h"
#include "shared/vehicle.h"

#include <filesystem>
#include <functional>
#include <string>

namespace RDMP {
    inline std::shared_ptr<spdlog::logger> Log() {
        return Framework::Logging::GetLogger("RDMP");
    }

    class FwClient: public Framework::Integrations::Client::Instance {
      public:
        bool connected = false;
        float tickMs   = 0.0f; // server tick interval (ms), from the handshake

        // Set by the in-game DLL: publishes the RDMP client script natives into each script context, and sees chat.
        std::function<void(Framework::Scripting::Engine *)> onModuleRegister;
        std::function<void(const std::string &author, const std::string &text, uint32_t color)> onChatLine;

        // Round trip to the server (ms), -1 = unknown.
        int PingMs() const {
            auto *peer = Peer();
            if (!peer || peer->NumberOfConnections() == 0) {
                return -1;
            }
            return peer->GetAveragePing(peer->GetSystemAddressFromIndex(0));
        }

        void PostInit() override {
            Framework::Networking::Replication::EntityRegistry::Get().Register<Shared::Player>(Shared::Player::kTypeName);
            Framework::Networking::Replication::EntityRegistry::Get().Register<Shared::Blip>(Shared::Blip::kTypeName);
            Framework::Networking::Replication::EntityRegistry::Get().Register<Shared::Vehicle>(Shared::Vehicle::kTypeName);
        }

        void ModuleRegister(Framework::Scripting::Engine *engine) override {
            if (onModuleRegister) {
                onModuleRegister(engine);
            }
        }

        // The framework emits the "chatMessage" script event itself; our chat box (hud.cpp) shows the line.
        void OnChatMessageReceived(const Framework::Networking::RPC::ChatMessage &msg) override {
            if (onChatLine) {
                onChatLine(msg.author, msg.text, msg.color);
            }
        }

        void OnConnectionFinalized(float serverTickRate) override {
            connected = true;
            tickMs    = serverTickRate * 1000.0f; // the handshake carries the tick interval in seconds
            Log()->info("connected to the server (tick rate {})", serverTickRate);
        }

        void OnConnectionClosed() override {
            connected = false;
            Log()->warn("disconnected from the server");
        }

        bool Connect(const std::string &host, int32_t port, const std::string &nickname, const std::string &password = {}) {
            auto st     = GetCurrentState();
            st.host     = host;
            st.port     = port;
            st.nickname = nickname;
            SetCurrentState(st);
            Log()->info("connecting to {}:{} as '{}' ...", host, port, nickname);
            return static_cast<bool>(ConnectToServer(host, port, password));
        }

        // Our own avatar (the server hands ownership to the connection), nullptr until assigned.
        Shared::Player *Owned() const {
            Shared::Player *mine = nullptr;
            if (auto *replication = Replication()) {
                Framework::Networking::Replication::ClientEntities(*replication).ForEach<Shared::Player>(
                    [&mine](Shared::Player *player) {
                        if (player->IsOwner()) {
                            mine = player;
                        }
                    });
            }
            return mine;
        }

        // Every remote avatar currently streamed to us.
        template <typename Fn>
        void ForEachRemote(Fn &&fn) const {
            if (auto *replication = Replication()) {
                Framework::Networking::Replication::ClientEntities(*replication).ForEach<Shared::Player>(
                    [&fn](Shared::Player *player) {
                        if (!player->IsOwner()) {
                            fn(player);
                        }
                    });
            }
        }

        // Every script blip currently replicated to us (server-owned RDMP::Blip entities).
        template <typename Fn>
        void ForEachBlip(Fn &&fn) const {
            if (auto *replication = Replication()) {
                Framework::Networking::Replication::ClientEntities(*replication).ForEach<Shared::Blip>(
                    [&fn](Shared::Blip *blip) {
                        fn(blip);
                    });
            }
        }

        // Every vehicle currently streamed to us (RDMP::Vehicle).
        template <typename Fn>
        void ForEachVehicle(Fn &&fn) const {
            if (auto *replication = Replication()) {
                Framework::Networking::Replication::ClientEntities(*replication).ForEach<Shared::Vehicle>(
                    [&fn](Shared::Vehicle *vehicle) {
                        fn(vehicle);
                    });
            }
        }

        template <typename T>
        T *Entity(uint64_t networkId) const {
            auto *replication = Replication();
            return replication ? Framework::Networking::Replication::ClientEntities(*replication).GetEntity<T>(networkId) : nullptr;
        }

        // In game: the renderer and ImGui come up later, once the overlay has seen the game's swapchain
        // (Overlay::Frame -> RenderInit). Until then this behaves like InitHeadless.
        bool InitInGame(const std::string &cacheRoot) {
            return InitWith(cacheRoot, true);
        }

        bool InitHeadless(const std::string &cacheRoot = {}) {
            return InitWith(cacheRoot, false);
        }

      private:
        bool InitWith(const std::string &cacheRoot, bool renderer) {
            // The framework's chat box timestamps every received line with ImGui::GetTime() even when nothing
            // renders it; a headless client has no ImGui context, so the first chat line (the wasted script's
            // "was wasted" announcement) dereferenced null and killed the game. A bare context is all it needs.
            if (!ImGui::GetCurrentContext()) {
                ImGui::CreateContext(nullptr);
            }
            Framework::Integrations::Client::InstanceOptions opts;
            opts.gameName             = "Red Dead Redemption";
            opts.gameVersion          = "1.0.42.49788435";
            opts.modSlug              = "rdmp"; // must match the server's slug (build token)
            opts.modVersion           = "0.5.0";
            opts.cacheRoot            = cacheRoot;
            opts.useRenderer          = renderer; // headless (bot): no D3D / ImGui at all
            opts.useImGUI             = renderer;
            opts.usePresence          = false; // no Discord
            opts.initRendererManually = true;
            opts.verifyBuildToken     = true; // must match the server: same game/mod identity, same salt
            if (renderer) {
                // The UI's type (menu, chat, nametags): Segoe UI in four weights, Segoe MDL2 for icons; all ship with
                // Windows 10 / 11. A missing one falls back to ImGui's own font.
                wchar_t windows[MAX_PATH] {};
                GetWindowsDirectoryW(windows, MAX_PATH);
                const std::filesystem::path fonts = std::filesystem::path(windows) / "Fonts";
                auto font = [&](const char *file) {
                    const auto path = fonts / file;
                    return std::filesystem::exists(path) ? path.string() : std::string();
                };
                opts.imguiFontPath = font("segoeui.ttf");
                opts.imguiFontSize = 18.0f;
                for (const auto &[name, file] : {std::pair {"semibold", "seguisb.ttf"}, std::pair {"bold", "segoeuib.ttf"}, std::pair {"black", "seguibl.ttf"},
                                                  std::pair {"icons", "segmdl2.ttf"}}) {
                    if (const std::string path = font(file); !path.empty()) {
                        opts.imguiFonts.push_back({name, path});
                    }
                }
            }
            if (auto res = Init(opts); !res) {
                Log()->error("client init failed: {}", res.GetError().message);
                return false;
            }
            return true;
        }

      public:

        Framework::Networking::Replication::ReplicationManager *Replication() const {
            auto *client = Networking();
            return client ? client->GetReplicationManager() : nullptr;
        }

      private:
        Framework::Networking::NetworkClient *Networking() const {
            auto *engine = GetNetworkingEngine();
            return engine ? engine->GetNetworkClient() : nullptr;
        }
        MafiaNet::RakPeerInterface *Peer() const {
            auto *client = Networking();
            return client ? client->GetPeer() : nullptr;
        }
    };
} // namespace RDMP
