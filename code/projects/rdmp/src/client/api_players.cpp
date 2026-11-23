/*
 * CLIENT player and element functions:
 *   getLocalPlayer() -> element | null
 *   getLocalActor() -> number                       the local player's script Actor handle (for invokeNative), 0 = none
 *   getElementPosition(element) -> {x, y, z}        (local player: live game position)
 *   setElementPosition(localPlayer, x, y, z)        the local player only
 *   getElementHealth(element) / setElementHealth(localPlayer, hp)
 *   killPed(localPlayer)                            the game's death (animation); the server then hears playerWasted
 *   getPlayerName(player), getElementType(element), isElement(value), getElementsByType("player" | "blip")
 *   fadeCamera(fadeIn[, ms]), setCameraTarget(), toggleAllControls(enabled)
 *   getPedWeapon(player) -> weapon id in hand (-1 = none), getPedTotalAmmo(player) -> its ammo
 *   getWeaponNameFromID(id) / getWeaponIDFromName(name)                  shared/weapons.h
 */

#include <utils/safe_win32.h>

#include "client/api.h"
#include "client/fw_client.h"
#include "shared/weapons.h"

namespace RDMP::ClientApi {
    namespace {
        using namespace ScriptApi;

        void GetLocalPlayerFn(const Args &args) {
            args.GetReturnValue().Set(Wrap(args.GetIsolate(), LocalId()));
        }

        void GetLocalActorFn(const Args &args) {
            float pos[3];
            uint32_t actor = 0;
            LocalPlayer(pos, nullptr, nullptr, &actor);
            args.GetReturnValue().Set(actor);
        }

        void GetElementPositionFn(const Args &args) {
            auto *isolate     = args.GetIsolate();
            const uint64_t id = ElementId(args, 0);
            float p[3]        = {};
            if (id && id == LocalId() && LocalPlayer(p)) {
                args.GetReturnValue().Set(Vec3(isolate, p[0], p[1], p[2]));
                return;
            }
            if (auto *player = Client() ? Client()->Entity<Shared::Player>(id) : nullptr) {
                args.GetReturnValue().Set(Vec3(isolate, player->position.x, player->position.y, player->position.z));
                return;
            }
            if (auto *blip = Client() ? Client()->Entity<Shared::Blip>(id) : nullptr) {
                args.GetReturnValue().Set(Vec3(isolate, blip->position.x, blip->position.y, blip->position.z));
                return;
            }
            Throw(isolate, "getElementPosition(element) -> {x, y, z}");
        }

        void SetElementPositionFn(const Args &args) {
            if (ElementId(args, 0) != LocalId() || args.Length() < 4 || !Host()) {
                Throw(args.GetIsolate(), "setElementPosition(localPlayer, x, y, z)");
                return;
            }
            float p[3] = {}, heading = 0.0f;
            LocalPlayer(p, &heading);
            const float to[3] = {Float(args, 1, p[0]), Float(args, 2, p[1]), Float(args, 3, p[2])};
            Host()->teleport_local(to, heading);
            args.GetReturnValue().Set(true);
        }

        void GetElementHealthFn(const Args &args) {
            const uint64_t id = ElementId(args, 0);
            float p[3] = {}, hp = 0.0f;
            if (id && id == LocalId() && LocalPlayer(p, nullptr, &hp)) {
                args.GetReturnValue().Set(static_cast<double>(hp));
                return;
            }
            auto *player = Client() ? Client()->Entity<Shared::Player>(id) : nullptr;
            args.GetReturnValue().Set(player ? static_cast<int>(player->localHealth) : 0);
        }

        // The weapon in a player's hand as the network has it (the local player's own state included).
        void GetPedWeaponFn(const Args &args) {
            auto *player = Client() ? Client()->Entity<Shared::Player>(ElementId(args, 0)) : nullptr;
            args.GetReturnValue().Set(player ? static_cast<int>(player->weapon) : -1);
        }

        void GetPedTotalAmmoFn(const Args &args) {
            auto *player = Client() ? Client()->Entity<Shared::Player>(ElementId(args, 0)) : nullptr;
            args.GetReturnValue().Set(player && player->weapon >= 0 ? static_cast<int>(player->ammo) : 0);
        }

        void GetWeaponNameFn(const Args &args) {
            const int id = static_cast<int>(Num(args, 0, -1));
            if (rdmp::weapons::Valid(id)) {
                args.GetReturnValue().Set(JsString(args.GetIsolate(), rdmp::weapons::Name(id)));
            }
            else {
                args.GetReturnValue().SetNull();
            }
        }

        void GetWeaponIdFn(const Args &args) {
            const int id = Has(args, 0) && args[0]->IsString() ? rdmp::weapons::FromName(Str(args.GetIsolate(), args[0])) : -1;
            args.GetReturnValue().Set(id);
        }

        void SetElementHealthFn(const Args &args) {
            if (ElementId(args, 0) != LocalId() || !Host()) {
                Throw(args.GetIsolate(), "setElementHealth(localPlayer, health)");
                return;
            }
            Host()->set_health(Float(args, 1, 100.0f));
            args.GetReturnValue().Set(true);
        }

        void KillPedFn(const Args &args) {
            if (ElementId(args, 0) != LocalId() || !Host()) {
                Throw(args.GetIsolate(), "killPed(localPlayer)");
                return;
            }
            Host()->kill_local();
            args.GetReturnValue().Set(true);
        }

        void GetPlayerNameFn(const Args &args) {
            auto *player = Client() ? Client()->Entity<Shared::Player>(ElementId(args, 0)) : nullptr;
            args.GetReturnValue().Set(JsString(args.GetIsolate(), player ? player->nickname : std::string()));
        }

        void GetElementTypeFn(const Args &args) {
            auto *isolate     = args.GetIsolate();
            const uint64_t id = ElementId(args, 0);
            if (Client() && Client()->Entity<Shared::Player>(id)) {
                args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "player"));
            }
            else if (Client() && Client()->Entity<Shared::Blip>(id)) {
                args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "blip"));
            }
            else {
                args.GetReturnValue().Set(false);
            }
        }

        void IsElementFn(const Args &args) {
            const uint64_t id = ElementId(args, 0);
            args.GetReturnValue().Set(Client() && id && (Client()->Entity<Shared::Player>(id) || Client()->Entity<Shared::Blip>(id)));
        }

        void GetElementsByTypeFn(const Args &args) {
            auto *isolate = args.GetIsolate();
            if (!Has(args, 0) || !args[0]->IsString() || !Client()) {
                Throw(isolate, "getElementsByType(\"player\" | \"blip\") -> [elements]");
                return;
            }
            const std::string type = Str(args, 0);
            std::vector<uint64_t> ids;
            if (type == "player") {
                if (const uint64_t me = LocalId()) {
                    ids.push_back(me);
                }
                Client()->ForEachRemote([&](Shared::Player *p) { ids.push_back(p->GetNetworkID()); });
            }
            else if (type == "blip") {
                Client()->ForEachBlip([&](Shared::Blip *b) { ids.push_back(b->GetNetworkID()); });
            }
            auto context = isolate->GetCurrentContext();
            auto result  = v8::Array::New(isolate, static_cast<int>(ids.size()));
            for (uint32_t i = 0; i < ids.size(); ++i) {
                result->Set(context, i, Wrap(isolate, ids[i])).Check();
            }
            args.GetReturnValue().Set(result);
        }

        void FadeCameraFn(const Args &args) {
            if (Host()) {
                Host()->fade(Bool(args, 0, true) ? 1 : 0, Float(args, 1, 1000.0f));
            }
        }

        void SetCameraTargetFn(const Args &) {
            if (Host()) {
                Host()->camera_to_player();
            }
        }

        void ToggleAllControlsFn(const Args &args) {
            if (Host()) {
                Host()->set_control(Bool(args, 0, true) ? 1 : 0);
            }
        }
    } // namespace

    void RegisterPlayerApi(const ScriptApi::Publisher &publish) {
        publish("getLocalPlayer", &GetLocalPlayerFn);
        publish("getLocalActor", &GetLocalActorFn);
        publish("getElementPosition", &GetElementPositionFn);
        publish("setElementPosition", &SetElementPositionFn);
        publish("getElementHealth", &GetElementHealthFn);
        publish("setElementHealth", &SetElementHealthFn);
        publish("killPed", &KillPedFn);
        publish("getPlayerName", &GetPlayerNameFn);
        publish("getElementType", &GetElementTypeFn);
        publish("isElement", &IsElementFn);
        publish("getElementsByType", &GetElementsByTypeFn);
        publish("fadeCamera", &FadeCameraFn);
        publish("setCameraTarget", &SetCameraTargetFn);
        publish("toggleAllControls", &ToggleAllControlsFn);
        publish("getPedWeapon", &GetPedWeaponFn);
        publish("getPedTotalAmmo", &GetPedTotalAmmoFn);
        publish("getWeaponNameFromID", &GetWeaponNameFn);
        publish("getWeaponIDFromName", &GetWeaponIdFn);
    }
} // namespace RDMP::ClientApi
