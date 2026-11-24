/*
 * SERVER weapon functions (MTA names; weapon = an id 0..31 or a name, shared/weapons.h):
 *   giveWeapon(player, weapon[, ammo = 30[, inHand = false]])     -> bool
 *   takeWeapon(player, weapon) / takeAllWeapons(player)            -> bool
 *   setWeaponAmmo(player, weapon, ammo)                            -> bool
 *   setPedWeapon(player, weapon)                                   put it in hand (-1 / null = holster)
 *   getPedWeapon(player)                                           the weapon in hand, -1 = none
 *   getPedTotalAmmo(player)                                        ammo of the weapon in hand
 *   getPedWeapons(player)                                          [weapon ids] the server gave and did not take
 *   getWeaponNameFromID(id) / getWeaponIDFromName(name)
 * Events: playerWeaponSwitch(player, previous, current)  (ids, -1 = none; the game holsters after a few seconds)
 *         playerDamage(player, attacker | null, weapon, bodyPart, loss)   the victim's game decided it (as in MTA)
 *         playerWasted(player, killer | null, weapon, bodyPart)            killer = the last hit within 5 s
 */

#include "server/server.h"

#include "shared/weapons.h"

#include <chrono>
#include <map>
#include <set>
#include <unordered_map>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;
        namespace W = rdmp::weapons;

        std::unordered_map<uint64_t, std::set<int>> g_given; // guarded by StateMutex()
        std::unordered_map<uint64_t, int> g_lastWeapon;      // guarded by StateMutex()
        struct Hit {
            uint8_t seq = 0;
            bool seen = false;
            uint64_t attacker = 0;
            int weapon = -1, bodyPart = -1;
            std::chrono::steady_clock::time_point at {};
        };
        std::unordered_map<uint64_t, Hit> g_hits;            // guarded by StateMutex()

        // A weapon argument: an id or a name; -2 when it is neither.
        int WeaponArg(const Args &args, int i) {
            if (!Has(args, i)) {
                return -2;
            }
            if (args[i]->IsNumber()) {
                const int id = static_cast<int>(Num(args, i, -2));
                return W::Valid(id) ? id : -2;
            }
            if (args[i]->IsString()) {
                const int id = W::FromName(Str(args.GetIsolate(), args[i]));
                return id >= 0 ? id : -2;
            }
            return -2;
        }

        void GiveWeaponFn(const Args &args) {
            auto player  = PlayerFromArg(args);
            const int id = WeaponArg(args, 1);
            if (!player || id < 0) {
                Throw(args.GetIsolate(), "giveWeapon(player, weapon[, ammo[, inHand]])");
                return;
            }
            const float ammo = std::max(0.0f, Float(args, 2, 30.0f));
            QueueAction(player.Id(), ActionKind::GiveWeapon, static_cast<float>(id), ammo, Bool(args, 3, false) ? 1.0f : 0.0f);
            {
                std::lock_guard lock(StateMutex());
                g_given[player.Id()].insert(id);
            }
            args.GetReturnValue().Set(true);
        }

        void TakeWeaponFn(const Args &args) {
            auto player  = PlayerFromArg(args);
            const int id = WeaponArg(args, 1);
            if (!player || id < 0) {
                Throw(args.GetIsolate(), "takeWeapon(player, weapon)");
                return;
            }
            QueueAction(player.Id(), ActionKind::TakeWeapon, static_cast<float>(id));
            {
                std::lock_guard lock(StateMutex());
                g_given[player.Id()].erase(id);
            }
            args.GetReturnValue().Set(true);
        }

        void TakeAllWeaponsFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "takeAllWeapons(player)");
                return;
            }
            QueueAction(player.Id(), ActionKind::TakeWeapon, -1.0f);
            {
                std::lock_guard lock(StateMutex());
                g_given.erase(player.Id());
            }
            args.GetReturnValue().Set(true);
        }

        void SetWeaponAmmoFn(const Args &args) {
            auto player  = PlayerFromArg(args);
            const int id = WeaponArg(args, 1);
            if (!player || id < 0 || !Has(args, 2)) {
                Throw(args.GetIsolate(), "setWeaponAmmo(player, weapon, ammo)");
                return;
            }
            QueueAction(player.Id(), ActionKind::WeaponAmmo, static_cast<float>(id), std::max(0.0f, Float(args, 2, 0)));
            args.GetReturnValue().Set(true);
        }

        void SetPedWeaponFn(const Args &args) {
            auto player = PlayerFromArg(args);
            if (!player) {
                Throw(args.GetIsolate(), "setPedWeapon(player, weapon)  (-1 or null holsters)");
                return;
            }
            const bool holster = !Has(args, 1) || args[1]->IsNull() || (args[1]->IsNumber() && Num(args, 1, -1) < 0);
            const int id       = holster ? -1 : WeaponArg(args, 1);
            if (!holster && id < 0) {
                Throw(args.GetIsolate(), "setPedWeapon: unknown weapon");
                return;
            }
            QueueAction(player.Id(), ActionKind::WeaponInHand, static_cast<float>(id));
            args.GetReturnValue().Set(true);
        }

        void GetPedWeaponFn(const Args &args) {
            auto player = PlayerFromArg(args);
            args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), player ? player->weapon : -1));
        }

        void GetPedTotalAmmoFn(const Args &args) {
            auto player = PlayerFromArg(args);
            args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), player && player->weapon >= 0 ? player->ammo : 0));
        }

        void GetPedWeaponsFn(const Args &args) {
            auto *isolate = args.GetIsolate();
            auto player   = PlayerFromArg(args);
            std::vector<int> ids;
            if (player) {
                std::lock_guard lock(StateMutex());
                const auto it = g_given.find(player.Id());
                if (it != g_given.end()) {
                    ids.assign(it->second.begin(), it->second.end());
                }
            }
            auto out = v8::Array::New(isolate, static_cast<int>(ids.size()));
            for (size_t i = 0; i < ids.size(); ++i) {
                out->Set(isolate->GetCurrentContext(), static_cast<uint32_t>(i), v8::Integer::New(isolate, ids[i])).Check();
            }
            args.GetReturnValue().Set(out);
        }

        void GetWeaponNameFn(const Args &args) {
            const int id = static_cast<int>(Num(args, 0, -1));
            if (W::Valid(id)) {
                args.GetReturnValue().Set(JsString(args.GetIsolate(), W::Name(id)));
            }
            else {
                args.GetReturnValue().SetNull();
            }
        }

        void GetWeaponIdFn(const Args &args) {
            const int id = Has(args, 0) && args[0]->IsString() ? W::FromName(Str(args.GetIsolate(), args[0])) : -1;
            args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), id));
        }
    } // namespace

    void UpdateWeapons(World &world) {
        std::vector<std::tuple<uint64_t, int, int>> switches;
        struct Damage {
            uint64_t victim, attacker;
            int weapon, bodyPart;
            float loss;
        };
        std::vector<Damage> damages;
        world.ForEachRead<Player>([&](const auto &p) {
            std::lock_guard lock(StateMutex()); // entity lock -> mutex, the allowed order
            Hit &h = g_hits[p.Id()];
            if (!h.seen) {
                h.seen = true;
                h.seq  = p->dmgSeq;
            }
            else if (h.seq != p->dmgSeq) {
                h.seq      = p->dmgSeq;
                h.attacker = p->dmgAttacker;
                h.weapon   = p->dmgWeapon;
                h.bodyPart = p->dmgBodyPart;
                h.at       = std::chrono::steady_clock::now();
                damages.push_back({p.Id(), p->dmgAttacker, p->dmgWeapon, p->dmgBodyPart, p->dmgLoss});
            }
            const auto it = g_lastWeapon.find(p.Id());
            if (it == g_lastWeapon.end()) {
                g_lastWeapon[p.Id()] = p->weapon;
            }
            else if (it->second != p->weapon) {
                switches.emplace_back(p.Id(), it->second, p->weapon);
                it->second = p->weapon;
            }
        });
        for (const auto &[id, before, now] : switches) {
            EmitPlayerEventArgs("playerWeaponSwitch", id, {EventArg::Number(before), EventArg::Number(now)});
        }
        for (const Damage &d : damages) {
            EmitPlayerEventArgs("playerDamage", d.victim,
                                {EventArg::Element(d.attacker), EventArg::Number(d.weapon), EventArg::Number(d.bodyPart), EventArg::Number(d.loss)});
        }
    }

    void EmitWasted(uint64_t playerId) {
        uint64_t killer = 0;
        int weapon = -1, bodyPart = -1;
        {
            std::lock_guard lock(StateMutex());
            const auto it = g_hits.find(playerId);
            if (it != g_hits.end() && it->second.at != std::chrono::steady_clock::time_point {} &&
                std::chrono::steady_clock::now() - it->second.at < std::chrono::seconds(5)) {
                killer   = it->second.attacker;
                weapon   = it->second.weapon;
                bodyPart = it->second.bodyPart;
                it->second.at = {};
            }
        }
        EmitPlayerEventArgs("playerWasted", playerId, {EventArg::Element(killer), EventArg::Number(weapon), EventArg::Number(bodyPart)});
    }

    void ForgetPlayerWeapons(uint64_t playerId) {
        std::lock_guard lock(StateMutex());
        g_lastWeapon.erase(playerId);
        g_given.erase(playerId);
        g_hits.erase(playerId);
    }

    void RegisterWeaponApi(const ScriptApi::Publisher &publish) {
        publish("giveWeapon", &GiveWeaponFn);
        publish("takeWeapon", &TakeWeaponFn);
        publish("takeAllWeapons", &TakeAllWeaponsFn);
        publish("setWeaponAmmo", &SetWeaponAmmoFn);
        publish("setPedWeapon", &SetPedWeaponFn);
        publish("getPedWeapon", &GetPedWeaponFn);
        publish("getPedTotalAmmo", &GetPedTotalAmmoFn);
        publish("getPedWeapons", &GetPedWeaponsFn);
        publish("getWeaponNameFromID", &GetWeaponNameFn);
        publish("getWeaponIDFromName", &GetWeaponIdFn);
    }
} // namespace RDMP::Server
