/*
 * RDMP - Framework v44 port
 * Shared player entity (client and server compile the same header).
 *
 * Wire contract, three ways:
 *   construction -> nickname / model (sent once, reliable)
 *   transform    -> position, heading, speed, movement input (unreliable, every tick, owner-authored)
 *   fields       -> flags, health, ... (reliable deltas; health and flags are server fields)
 *
 * Server actions (script natives) use actSeq/actKind/actA..D (server fields, delivered to the owner by a
 * forced state) and actAck (owner field): the owner echoes the last actSeq it queued, and the server sends
 * the next action only after that ack, so back-to-back natives cannot overwrite each other in flight.
 */

#pragma once

#include <networking/replication/network_entity.h>

#include <cstdint>
#include <string>

namespace RDMP::Shared {
    using Framework::Networking::Replication::FieldSerializer;

    struct Player final: Framework::Networking::Replication::NetworkEntity {
        static constexpr const char *kTypeName = "RDMP::Player";
        static constexpr float kStreamRange    = 250.0f;
        static constexpr float kWorldMin       = -10000.0f;
        static constexpr float kWorldMax       = 10000.0f;
        static constexpr int kPosBits          = 22; // ~2.4 mm over the 20000 unit span
        static constexpr int kHeadingBits      = 16;
        static constexpr int kSpeedBits        = 8;
        static constexpr int kStickBits        = 8;
        static constexpr int kMoveHeadingBits  = 12;

        enum Flags : uint8_t {
            kAlive = 1u << 0,
            kAiming = 1u << 1,
            kOnHorse = 1u << 2,
            kInCover = 1u << 3,
        };

        // --- construction (spawn metadata) ---------------------------------
        std::string nickname;
        uint32_t def         = 0;  // actor definition index
        uint16_t variation   = 12; // model variation

        // --- transform ------------------------------------------------------
        // position comes from NetworkEntity::position (glm::vec3, Y up) so the framework's own
        // Entity script handle (getPosition/setPosition) drives the same state we replicate.
        float heading = 0.0f; // degrees, -180..180 (rotation is the yaw about Y)
        float speed   = 0.0f; // m/s, 0..60
        // Movement input (shared/types.h MoveInput): the puppet replays it, so it must arrive with its position.
        float moveRight   = 0.0f; // -1..1
        float moveBack    = 0.0f; // -1..1
        float moveHeading = 0.0f; // 0..2pi
        uint8_t gait      = 0;    // locomotion gait byte
        // Controller state (shared/types.h PlayerState::controls): every on-foot input, 8 bits each.
        static constexpr int kControls = 30;
        uint8_t controls[kControls] = {};
        // The world point the player's crosshair is on (shared/types.h PlayerState::aimX): remote puppets aim at it.
        float aimX = 0.0f, aimY = 0.0f, aimZ = 0.0f;
        // Their camera's position: where their shots start (shared/types.h PlayerState::camX).
        float camX = 0.0f, camY = 0.0f, camZ = 0.0f;
        // The weapon state as the owner's game has it (shared/types.h PlayerState::aimState): sent with every transform so
        // aiming, firing and reloading reach the puppet with the position's delay, no later.
        uint8_t aimState = 0, weaponFlags = 0, readyTicks = 0, shots = 0, clip = 0;

        // --- reliable delta fields -----------------------------------------
        uint8_t flags  = kAlive; // server field
        uint8_t health = 100;    // server field
        uint8_t armour = 0;

        // Server-requested client action (script native): a sequence number the owner watches, so
        // repeating the same action still fires. kind/spawn position follow src/shared/fw_api.h.
        uint8_t actSeq   = 0;
        uint8_t actKind  = 0;
        float actA       = 0.0f;
        float actB       = 0.0f;
        float actC       = 0.0f;
        float actD       = 0.0f;
        std::string actText;         // the action's text, for actions that carry one (HUD text)

        // Nametag (MTA setPlayerNametagText / Color / Showing), server fields every client reads.
        std::string nametagText;      // empty = the nickname
        uint32_t nametagColor  = 0;   // 0xAARRGGBB, 0 = default (white)
        uint8_t nametagShowing = 1;

        // The vehicle (shared/vehicle.h) the server has this player on (0 = on foot). Server field: the owner hears it
        // through a forced state and mounts / dismounts when it changes (warpPedIntoVehicle, removePedFromVehicle).
        uint64_t vehicle = 0;
        // Owner -> server: the vehicle the game has the player on (mounted with the game's own controls, or after a
        // warp). The server accepts it when the vehicle is free and makes the rider its owner.
        uint64_t localVehicle = 0;

        // Owner -> server: the last actSeq this client has queued for the game (see the header comment).
        uint8_t actAck = 0;

        // Owner -> every client: the weapon in hand (shared/weapons.h, -1 = none) and its ammo. Remote puppets carry the
        // same weapon; the synced aim / fire inputs (controls) make them use it.
        int8_t weapon   = -1;
        uint16_t ammo   = 0;

        // Owner -> server: the last hit the owner's game gave them (the victim's game decides damage, as in MTA): dmgSeq
        // counts hits (wraps), the rest describe the newest one. Server: playerDamage, and the killer on playerWasted.
        uint8_t dmgSeq       = 0;
        uint64_t dmgAttacker = 0;   // the attacking player's network id, 0 = none (fall, fire, NPC)
        int8_t dmgWeapon     = -1;  // shared/weapons.h, -1 = none
        int8_t dmgBodyPart   = -1;  // the game's hit zone, -1 = unknown
        float dmgLoss        = 0.0f;

        // Owner -> server: the game's real health (0 = dead). `health` above is a server field the owner never
        // sends up, so without this a real in-game death (fall, drowning, gunfire) never reached the server.
        uint8_t localHealth = 100;

        // Owner only, not replicated: set when a forced state carried a pose the server authored (a teleport),
        // as opposed to the owner's own pose echoed back with a forced state pushed for some other field.
        bool poseForcedPending = false;

        void OnSerializeConstruction(FieldSerializer &fields) override {
            fields.Field(nickname);
            fields.Field(def);
            fields.Field(variation);
        }

        void SerializeTransform(FieldSerializer &fields) override {
            fields.FloatBits(position.x, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.y, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.z, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(heading, -180.0f, 180.0f, kHeadingBits);
            fields.FloatBits(speed, 0.0f, 60.0f, kSpeedBits);
            fields.FloatBits(moveRight, -1.0f, 1.0f, kStickBits);
            fields.FloatBits(moveBack, -1.0f, 1.0f, kStickBits);
            fields.FloatBits(moveHeading, 0.0f, 6.2831853f, kMoveHeadingBits);
            fields.BitsRange<uint8_t>(gait, 0, 255);
            for (uint8_t &c : controls) {
                fields.BitsRange<uint8_t>(c, 0, 255);
            }
            fields.FloatBits(aimX, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(aimY, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(aimZ, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(camX, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(camY, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(camZ, kWorldMin, kWorldMax, kPosBits);
            fields.BitsRange<uint8_t>(aimState, 0, 7);
            fields.BitsRange<uint8_t>(weaponFlags, 0, 7);
            fields.BitsRange<uint8_t>(readyTicks, 0, 255);
            fields.BitsRange<uint8_t>(shots, 0, 255);
            fields.BitsRange<uint8_t>(clip, 0, 255);
        }

        void SerializeFields(FieldSerializer &fields) override {
            fields.ServerField(flags);
            fields.ServerField(health);
            fields.ServerField(armour);
            fields.ServerField(actSeq);
            fields.ServerField(actKind);
            fields.ServerField(actA);
            fields.ServerField(actB);
            fields.ServerField(actC);
            fields.ServerField(actD);
            fields.ServerField(actText);
            fields.ServerField(nametagText);
            fields.ServerField(nametagColor);
            fields.ServerField(nametagShowing);
            fields.Field(actAck);
            fields.Field(localHealth);
            fields.ServerField(vehicle);
            fields.Field(localVehicle);
            fields.Field(weapon);
            fields.Field(ammo);
            fields.Field(dmgSeq);
            fields.Field(dmgAttacker);
            fields.Field(dmgWeapon);
            fields.Field(dmgBodyPart);
            fields.Field(dmgLoss);
        }

        // The owner only hears a server field through a forced state, so repeat them here.
        void SerializeForcedState(FieldSerializer &fields) override {
            fields.Field(flags);
            fields.Field(health);
            fields.Field(armour);
            fields.Field(actSeq);
            fields.Field(actKind);
            fields.Field(actA);
            fields.Field(actB);
            fields.Field(actC);
            fields.Field(actD);
            fields.Field(actText);
            fields.Field(nametagText);
            fields.Field(nametagColor);
            fields.Field(nametagShowing);
            fields.Field(vehicle);
        }

        void OnStateForced() override {
            if (WasPoseForced()) {
                poseForcedPending = true;
            }
        }

        bool Alive() const {
            return (flags & kAlive) != 0;
        }
    };
} // namespace RDMP::Shared
