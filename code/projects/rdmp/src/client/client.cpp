/*
 * rdmp_client.dll - the MafiaHub Framework v44 client inside Red Dead Redemption.
 *
 * rdmp.dll (the game mod, its own build + static CRT) loads this at runtime and drives it from the GAME'S MAIN
 * THREAD once per frame through the C table in shared/fw_api.h. Client scripts therefore run on the main thread,
 * and their natives (client_natives.cpp) call into the game through the Host table rdmp.dll provides.
 */

#include <utils/safe_win32.h>

#include <integrations/client/networking/engine.h>
#include <networking/network_client.h>

#include "client/client_natives.h"
#include "client/fw_client.h"
#include "client/frontend.h"
#include "client/hud.h"
#include "client/overlay.h"
#include "shared/fw_api.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

namespace {
    using namespace rdmp;

    std::unique_ptr<RDMP::FwClient> g_client;
    const fw::Host *g_host = nullptr;
    net::PlayerState g_local {};
    float g_localHealth = 100.0f;
    bool g_haveLocal    = false;

    // A server teleport (forced state with a server-authored pose, Player::poseForcedPending), reported once.
    bool g_forcedPending = false;
    fw::Forced g_forced {};

    // Server-requested actions (script natives), queued for rdmp.dll to apply.
    constexpr int kActionQueue = 32;
    fw::Action g_actions[kActionQueue];
    int g_actionHead     = 0;
    int g_actionCount    = 0;
    uint8_t g_lastActSeq = 0;

    void Forward(const char *line) {
        if (g_host && g_host->log) {
            g_host->log(line);
        }
    }

    void Forwardf(const char *fmt, ...) {
        char line[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        Forward(line);
    }

    int Init(const char *logFolder, const char *cacheFolder, const fw::Host *host) {
        g_host = host;
        if (!host || host->version != fw::kApiVersion) {
            return 0;
        }
        try {
            auto *logging = Framework::Logging::GetInstance();
            if (logFolder && *logFolder) {
                logging->SetLogFolder(logFolder);
            }
            logging->SetLogName("rdmp_client_" + std::to_string(GetCurrentProcessId())); // one per game copy
            logging->SetLogForwarder(
                [](int level, const std::string &name, const std::string &message) {
                    static const char *kLevels[] = {"trace", "debug", "info", "warn", "error", "critical", "off"};
                    Forwardf("fw [%s] [%s] %s", kLevels[level >= 0 && level < 7 ? level : 2], name.c_str(), message.c_str());
                },
                spdlog::level::info);

            g_client = std::make_unique<RDMP::FwClient>();
            ClientNatives::Attach(g_client.get(), host);
            g_client->onModuleRegister = [](Framework::Scripting::Engine *engine) {
                ClientNatives::Register(engine);
            };
            g_client->onChatLine = [](const std::string &author, const std::string &text, uint32_t color) {
                // Our chat box once the overlay is up; until then (or without it) the game's subtitle line.
                if (RDMP::Overlay::Ready()) {
                    RDMP::Hud::ChatMessage(author, text, color);
                }
                else if (g_host && g_host->show_text) {
                    g_host->show_text(static_cast<int>(fw::TextStyle::Subtitle), (author.empty() ? text : author + ": " + text).c_str(), 6000);
                }
            };
            if (!g_client->InitInGame(cacheFolder ? cacheFolder : "")) {
                g_client.reset();
                return 0;
            }
            RDMP::Hud::Attach(g_client.get(), host);
            RDMP::Overlay::Install(g_client.get(), host);
            RDMP::Frontend::Attach(g_client.get(), host);
            return 1;
        }
        catch (const std::exception &e) {
            Forwardf("fw: init threw: %s", e.what());
            g_client.reset();
            return 0;
        }
    }

    int Connect(const char *address, int port, const char *nickname) {
        if (!g_client) {
            return 0;
        }
        return g_client->Connect(address ? address : "127.0.0.1", port, nickname ? nickname : "player") ? 1 : 0;
    }

    void Disconnect() {
        auto *net = g_client ? g_client->GetNetworkingEngine() : nullptr;
        if (auto *peer = net ? net->GetNetworkClient() : nullptr) {
            (void)peer->Disconnect();
        }
    }

    fw::Conn Update() {
        if (!g_client) {
            return fw::Conn::Disconnected;
        }
        try {
            // The owned entity can be destroyed and re-created (death, respawn, re-stream): Owned() may be null.
            if (auto *me = g_client->Owned()) {
                // A new server action: the sequence number changes for every request. Read even while dead: the
                // respawn flow (fade, health, spawnPlayer) arrives exactly then.
                if (me->actSeq != g_lastActSeq && me->actKind != 0 && g_actionCount < kActionQueue) {
                    g_lastActSeq       = me->actSeq;
                    fw::Action &action = g_actions[(g_actionHead + g_actionCount) % kActionQueue];
                    action.kind        = static_cast<fw::ActionKind>(me->actKind);
                    action.a           = me->actA;
                    action.b           = me->actB;
                    action.c           = me->actC;
                    action.d           = me->actD;
                    strncpy_s(action.text, me->actText.c_str(), _TRUNCATE);
                    ++g_actionCount;
                }
                me->actAck = g_lastActSeq; // the server sends the next action once it sees this

                if (me->poseForcedPending) {
                    me->poseForcedPending = false;
                    g_forced              = {me->position.x, me->position.y, me->position.z};
                    g_forcedPending       = true;
                    g_local.x = me->position.x, g_local.y = me->position.y, g_local.z = me->position.z;
                    Forwardf("server moved our avatar to (%.1f, %.1f, %.1f)", me->position.x, me->position.y, me->position.z);
                }
                if (g_haveLocal) {
                    me->localHealth = static_cast<uint8_t>(g_localHealth <= 0.0f ? 0.0f
                                                           : g_localHealth < 1.0f ? 1.0f
                                                           : g_localHealth > 255.0f ? 255.0f
                                                                                    : g_localHealth);
                    me->position = {g_local.x, g_local.y, g_local.z};
                    me->heading  = g_local.heading;
                    me->speed    = g_local.speed;
                    me->moveRight   = g_local.input.right;
                    me->moveBack    = g_local.input.back;
                    me->moveHeading = g_local.input.heading;
                    me->gait        = g_local.input.gait;
                    std::memcpy(me->controls, g_local.controls, sizeof(me->controls));
                    me->localVehicle = g_local.vehicle;
                    me->weapon       = g_local.weapon;
                    me->ammo         = g_local.ammo;
                    me->aimX = g_local.aimX, me->aimY = g_local.aimY, me->aimZ = g_local.aimZ;
                    me->aimState = g_local.aimState, me->weaponFlags = g_local.weaponFlags, me->readyTicks = g_local.readyTicks;
                    me->shots = g_local.shots, me->clip = g_local.clip;
                }
            }

            ClientNatives::BeforeUpdate(); // key/click/render events for client scripts (they queue dx* drawing)
            if (g_host->set_input_lock) { // typing in the chat must not move the player
                static bool locked = false;
                const bool typing  = RDMP::Overlay::UiHasKeyboard();
                if (typing != locked) {
                    locked = typing;
                    g_host->set_input_lock(typing ? 1 : 0);
                }
            }
            RDMP::Frontend::Frame(); // main menu state (shown while not in a session, Esc in one)
            RDMP::Overlay::Frame([] { // renderer bring-up, then this frame's ImGui widgets: HUD, the menu over it
                RDMP::Hud::Draw();
                RDMP::Frontend::Draw();
            });
            g_client->Update();             // networking, replication, client scripts, ImGui frame
        }
        catch (const std::exception &e) {
            Forwardf("fw: update threw: %s", e.what());
        }

        if (g_client->connected) {
            return fw::Conn::Connected;
        }
        using Phase = Framework::Integrations::Client::ConnectionPhase;
        return g_client->GetConnectionPhase() == Phase::Disconnected ? fw::Conn::Disconnected : fw::Conn::Connecting;
    }

    void SetLocal(const net::PlayerState *state, float health) {
        if (!state) {
            return;
        }
        g_local       = *state;
        g_localHealth = health;
        g_haveLocal   = true;
    }

    int GetRemotes(fw::Remote *out, int max) {
        if (!out || max <= 0 || !g_client) {
            return 0;
        }
        int n = 0;
        g_client->ForEachRemote([&](RDMP::Shared::Player *player) {
            if (n >= max) {
                return;
            }
            fw::Remote &remote = out[n++];
            remote             = fw::Remote {};
            remote.netId       = player->GetNetworkID();
            std::strncpy(remote.name, player->nickname.c_str(), sizeof(remote.name) - 1);
            // One sample per transform update the Framework applied: its send time (moved onto our clock on receipt)
            // numbers and times the sample, so rdmp.dll's interpolation buffer takes each new pose exactly once.
            const auto sent  = static_cast<uint32_t>(player->GetLastTransformTime());
            remote.s.seq     = sent;
            remote.s.timeMs  = sent ? sent : 1;
            remote.s.x       = player->position.x;
            remote.s.y       = player->position.y;
            remote.s.z       = player->position.z;
            remote.s.heading = player->heading;
            remote.s.speed   = player->speed;
            remote.health    = static_cast<float>(player->localHealth);
            // The quantized stick has no exact zero: snap the released stick back to 0.
            auto stick = [](float v) { return std::fabs(v) < 0.01f ? 0.0f : v; };
            remote.s.input.right   = stick(player->moveRight);
            remote.s.input.back    = stick(player->moveBack);
            remote.s.input.heading = player->moveHeading;
            remote.s.input.gait    = player->gait;
            std::memcpy(remote.s.controls, player->controls, sizeof(remote.s.controls));
            remote.s.vehicle = player->vehicle;
            remote.s.weapon  = player->weapon;
            remote.s.ammo    = player->ammo;
            remote.s.aimX = player->aimX, remote.s.aimY = player->aimY, remote.s.aimZ = player->aimZ;
            remote.s.aimState = player->aimState, remote.s.weaponFlags = player->weaponFlags, remote.s.readyTicks = player->readyTicks;
            remote.s.shots = player->shots, remote.s.clip = player->clip;
        });
        return n;
    }

    int PollForced(fw::Forced *out) {
        if (!g_forcedPending || !out) {
            return 0;
        }
        *out            = g_forced;
        g_forcedPending = false;
        return 1;
    }

    int PollAction(fw::Action *out) {
        if (g_actionCount <= 0 || !out) {
            return 0;
        }
        *out         = g_actions[g_actionHead];
        g_actionHead = (g_actionHead + 1) % kActionQueue;
        --g_actionCount;
        return 1;
    }

    void GetLink(fw::Link *out) {
        if (!out) {
            return;
        }
        *out = fw::Link {};
        if (!g_client) {
            return;
        }
        out->pingMs = g_client->PingMs();
        out->tickMs = g_client->tickMs;
    }

    int GetBlips(fw::Blip *out, int max) {
        if (!out || max <= 0 || !g_client) {
            return 0;
        }
        int n = 0;
        g_client->ForEachBlip([&](RDMP::Shared::Blip *blip) {
            if (n >= max) {
                return;
            }
            fw::Blip &b   = out[n++];
            b             = fw::Blip {};
            b.netId       = blip->GetNetworkID();
            b.x           = blip->position.x;
            b.y           = blip->position.y;
            b.z           = blip->position.z;
            b.attachedTo  = blip->attachedTo;
            b.rgba        = blip->rgba;
            b.scale       = blip->scale;
            b.maxDistance = blip->maxDistance;
            b.priority    = blip->priority;
            b.icon        = blip->icon;
            b.flags       = blip->flags;
        });
        return n;
    }

    int GetVehicles(fw::Vehicle *out, int max) {
        if (!out || max <= 0 || !g_client) {
            return 0;
        }
        int n = 0;
        g_client->ForEachVehicle([&](RDMP::Shared::Vehicle *v) {
            if (n >= max) {
                return;
            }
            fw::Vehicle &o = out[n++];
            o              = fw::Vehicle {};
            o.netId        = v->GetNetworkID();
            o.model        = v->model;
            o.x = v->position.x, o.y = v->position.y, o.z = v->position.z;
            o.heading       = v->heading;
            o.speed         = v->speed;
            auto stick      = [](float f) { return std::fabs(f) < 0.01f ? 0.0f : f; };
            o.input.right   = stick(v->moveRight);
            o.input.back    = stick(v->moveBack);
            o.input.heading = v->moveHeading;
            o.input.gait    = v->gait;
            o.occupant      = v->occupant;
            o.timeMs        = static_cast<uint32_t>(v->GetLastTransformTime());
            o.mine          = v->IsOwner() ? 1 : 0;
        });
        return n;
    }

    void SetVehicle(uint64_t netId, const float pos[3], float heading, float speed, const net::MoveInput *input) {
        if (!g_client || !pos) {
            return;
        }
        auto *v = g_client->Entity<RDMP::Shared::Vehicle>(netId);
        if (!v || !v->IsOwner()) {
            return;
        }
        v->position = {pos[0], pos[1], pos[2]};
        v->heading  = heading;
        v->speed    = speed;
        if (input) {
            v->moveRight   = input->right;
            v->moveBack    = input->back;
            v->moveHeading = input->heading;
            v->gait        = input->gait;
        }
    }

    uint64_t GetAssignedVehicle() {
        auto *me = g_client ? g_client->Owned() : nullptr;
        return me ? me->vehicle : 0;
    }

    uint64_t GetLocalId() {
        if (!g_client) {
            return 0;
        }
        auto *me = g_client->Owned();
        return me ? static_cast<uint64_t>(me->GetNetworkID()) : 0;
    }

    void Shutdown() {
        if (!g_client) {
            return;
        }
        try {
            g_client->Shutdown();
        }
        catch (...) {
        }
        ClientNatives::Attach(nullptr, nullptr);
        g_client.reset();
    }
} // namespace

extern "C" __declspec(dllexport) int rdmp_client_get_api(rdmp::fw::Api *out) {
    if (!out) {
        return 0;
    }
    out->version      = rdmp::fw::kApiVersion;
    out->init         = &Init;
    out->connect      = &Connect;
    out->update       = &Update;
    out->set_local    = &SetLocal;
    out->get_remotes  = &GetRemotes;
    out->poll_forced  = &PollForced;
    out->poll_action  = &PollAction;
    out->shutdown     = &Shutdown;
    out->get_link     = &GetLink;
    out->get_blips    = &GetBlips;
    out->get_local_id = &GetLocalId;
    out->get_vehicles = &GetVehicles;
    out->set_vehicle  = &SetVehicle;
    out->get_assigned_vehicle = &GetAssignedVehicle;
    out->browser_refresh  = [](const char *list) { RDMP::Frontend::BrowserRefresh(list); };
    out->browser_poll     = [](rdmp::fw::BrowserServer *o, int max) { return RDMP::Frontend::BrowserPoll(o, max); };
    out->browser_favorite = [](const char *host, int port, const char *name, int add) { RDMP::Frontend::BrowserFavorite(host, port, name, add != 0); };
    out->browser_connect  = [](const char *host, int port, const char *pw) { return RDMP::Frontend::BrowserConnect(host, port, pw) ? 1 : 0; };
    out->disconnect       = &Disconnect;
    out->get_settings     = [](rdmp::fw::Settings *o) { RDMP::Frontend::GetSettings(o); };
    out->set_settings     = [](const rdmp::fw::Settings *in) {
        if (in) {
            RDMP::Frontend::SetSettings(*in);
        }
    };
    return 1;
}
