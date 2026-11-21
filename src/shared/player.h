/*
 * RDMP - Framework v44 port
 * Shared player entity (client and server compile the same header).
 *
 * Wire contract, three ways:
 *   construction -> nickname / model (sent once, reliable)
 *   transform    -> position, heading, speed (unreliable, every tick, owner-authored)
 *   fields       -> gait, flags, health (reliable deltas; health and flags are server fields)
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

        enum Flags : uint8_t {
            kAlive = 1u << 0,
            kAiming = 1u << 1,
            kOnHorse = 1u << 2,
            kInCover = 1u << 3,
        };

        enum Gait : uint8_t {
            kIdle = 0,
            kWalk = 1,
            kRun  = 2,
            kSprint = 3,
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

        // --- reliable delta fields -----------------------------------------
        uint8_t gait   = kIdle;
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

        // Sparse locomotion desire: indices into the engine's 0x44-word desire block and
        // their values. Drives the puppet's walk/run without touching its position.
        static constexpr int kMaxDesire = 20;
        uint8_t desireCount             = 0;
        uint16_t desireIdx[kMaxDesire]  = {};
        uint32_t desireVal[kMaxDesire]  = {}; // raw desire words (bit patterns; a float field rounded them)

        // Owner -> server: the last actSeq this client has queued for the game (see the header comment).
        uint8_t actAck = 0;

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
        }

        void SerializeFields(FieldSerializer &fields) override {
            fields.Field(gait);
            fields.ServerField(flags);
            fields.ServerField(health);
            fields.ServerField(armour);
            fields.ServerField(actSeq);
            fields.ServerField(actKind);
            fields.ServerField(actA);
            fields.ServerField(actB);
            fields.ServerField(actC);
            fields.ServerField(actD);
            fields.Field(desireCount);
            for (int i = 0; i < kMaxDesire; ++i) {
                fields.Field(desireIdx[i]);
                fields.Field(desireVal[i]);
            }
            fields.Field(actAck);
            fields.Field(localHealth);
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
