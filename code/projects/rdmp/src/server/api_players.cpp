/*
 * SERVER player functions (MTA names):
 *   spawnPlayer(player, x, y, z[, heading])      create / respawn / move the player (full health, control on)
 *   fadeCamera(player, fadeIn[, ms = 1000])
 *   setCameraTarget(player)                      put the player's camera back behind them
 *   toggleAllControls(player, enabled)
 *   setElementHealth(player, hp) / getElementHealth(player)
 *   killPed(ped)                                 the game's death (animation), then playerWasted
 *   getPlayerName(player)
 *   setPlayerNametagText(player, text) / getPlayerNametagText(player)          "" = the player's name
 *   setPlayerNametagColor(player, r, g, b) / getPlayerNametagColor(player) -> [r, g, b]
 *   setPlayerNametagShowing(player, bool) / isPlayerNametagShowing(player)
 */

#include "server/server.h"

#include <algorithm>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;

        void SpawnPlayerFn(const Args &args) {
            uint64_t id = 0;
            {
                auto player = PlayerFromArg(args);
                if (!player || args.Length() < 4) {
                    Throw(args.GetIsolate(), "spawnPlayer(player, x, y, z[, heading])");
                    return;
                }
                player->flags  = static_cast<uint8_t>(player->flags | Player::kAlive);
                player->health = 100;
                id             = player.Id();
                QueueAction(id, ActionKind::Spawn, Float(args, 1, 0), Float(args, 2, 0), Float(args, 3, 0), Float(args, 4, 0));
            }
            MarkRespawned(id);
            EmitPlayerEvent("playerSpawned", id); // MTA's onPlayerSpawn
            args.GetReturnValue().Set(true);
        }

        void FadeCameraFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "fadeCamera(player, fadeIn[, ms])");
                return;
            }
            QueueAction(player.Id(), ActionKind::Fade, Bool(args, 1, true) ? 1.0f : 0.0f, Float(args, 2, 1000.0f));
            args.GetReturnValue().Set(true);
        }

        void SetCameraTargetFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "setCameraTarget(player)");
                return;
            }
            QueueAction(player.Id(), ActionKind::CameraTarget);
            args.GetReturnValue().Set(true);
        }

        void ToggleAllControlsFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "toggleAllControls(player, enabled)");
                return;
            }
            QueueAction(player.Id(), ActionKind::Control, Bool(args, 1, true) ? 1.0f : 0.0f);
            args.GetReturnValue().Set(true);
        }

        // health 0 is a scripted kill without the death animation (use killPed for the game's own death): the
        // game side never writes a real 0 (the engine would tear the actor down), so the server marks the death.
        void SetHealthFn(const Args &args) {
            uint64_t killed = 0;
            {
                auto player = PlayerFromArg(args);
                if (!player) {
                    Throw(args.GetIsolate(), "setElementHealth(player, health)");
                    return;
                }
                const float health = std::clamp(Float(args, 1, 100.0f), 0.0f, 255.0f);
                player->health     = static_cast<uint8_t>(health);
                QueueAction(player.Id(), ActionKind::Health, health);
                if (player->health == 0 && (player->flags & Player::kAlive)) {
                    player->flags = static_cast<uint8_t>(player->flags & ~Player::kAlive);
                    killed        = player.Id();
                    Log()->info("{} wasted via script", player->nickname);
                }
                else if (player->health > 0) {
                    player->flags = static_cast<uint8_t>(player->flags | Player::kAlive);
                }
            }
            if (killed) {
                MarkDead(killed);
                EmitPlayerEvent("playerWasted", killed);
            }
            args.GetReturnValue().Set(true);
        }

        // The game's real health as the client last reported it.
        void GetHealthFn(const Args &args) {
            auto player = PlayerFromArg(args);
            args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), player ? player->localHealth : 0));
        }

        void KillPedFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "killPed(ped)");
                return;
            }
            const bool alive = (player->flags & Player::kAlive) != 0;
            if (alive) {
                QueueAction(player.Id(), ActionKind::Kill); // the client's death monitor then reports playerWasted
            }
            args.GetReturnValue().Set(alive);
        }

        void GetNameFn(const Args &args) {
            auto player = PlayerFromArg(args);
            args.GetReturnValue().Set(JsString(args.GetIsolate(), player ? player->nickname : std::string()));
        }

        void SetNametagTextFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "setPlayerNametagText(player, text)");
                return;
            }
            player->nametagText = Str(args, 1);
            args.GetReturnValue().Set(true);
        }
        void GetNametagTextFn(const Args &args) {
            auto player = PlayerFromArg(args);
            const std::string text = player ? (player->nametagText.empty() ? player->nickname : player->nametagText) : std::string();
            args.GetReturnValue().Set(JsString(args.GetIsolate(), text));
        }
        void SetNametagColorFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "setPlayerNametagColor(player, r, g, b) / (player, false) = default");
                return;
            }
            player->nametagColor = (Has(args, 1) && args[1]->IsFalse()) ? 0u : ToColor(Num(args, 1, 255), Num(args, 2, 255), Num(args, 3, 255), 255);
            args.GetReturnValue().Set(true);
        }
        void GetNametagColorFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                args.GetReturnValue().Set(false);
                return;
            }
            const uint32_t c = player->nametagColor ? player->nametagColor : 0xFFFFFFFF;
            auto *isolate    = args.GetIsolate();
            auto context     = isolate->GetCurrentContext();
            auto result      = v8::Array::New(isolate, 3);
            result->Set(context, 0, v8::Integer::New(isolate, (c >> 16) & 0xFF)).Check();
            result->Set(context, 1, v8::Integer::New(isolate, (c >> 8) & 0xFF)).Check();
            result->Set(context, 2, v8::Integer::New(isolate, c & 0xFF)).Check();
            args.GetReturnValue().Set(result);
        }
        void SetNametagShowingFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "setPlayerNametagShowing(player, showing)");
                return;
            }
            player->nametagShowing = Bool(args, 1, true) ? 1 : 0;
            args.GetReturnValue().Set(true);
        }
        void IsNametagShowingFn(const Args &args) {
            auto player = PlayerFromArg(args);
            args.GetReturnValue().Set(player && player->nametagShowing != 0);
        }
    } // namespace

    void RegisterPlayerApi(const ScriptApi::Publisher &publish) {
        publish("spawnPlayer", &SpawnPlayerFn);
        publish("fadeCamera", &FadeCameraFn);
        publish("setCameraTarget", &SetCameraTargetFn);
        publish("toggleAllControls", &ToggleAllControlsFn);
        publish("setElementHealth", &SetHealthFn);
        publish("getElementHealth", &GetHealthFn);
        publish("killPed", &KillPedFn);
        publish("getPlayerName", &GetNameFn);
        publish("setPlayerNametagText", &SetNametagTextFn);
        publish("getPlayerNametagText", &GetNametagTextFn);
        publish("setPlayerNametagColor", &SetNametagColorFn);
        publish("getPlayerNametagColor", &GetNametagColorFn);
        publish("setPlayerNametagShowing", &SetNametagShowingFn);
        publish("isPlayerNametagShowing", &IsNametagShowingFn);
    }
} // namespace RDMP::Server
