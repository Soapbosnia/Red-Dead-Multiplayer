/*
 * RDMP - vehicle entity (MTA's "vehicle" element): a horse for now (an RDR rideable animal, actor def 976..1005).
 *
 * Server-owned while nobody rides it (the server's position is the truth, every client shows it standing still: no
 * animal AI). When a player mounts it the server makes the rider's client its owner, so the rider writes its pose and
 * the input the rider gives it (MTA: the driver syncs the vehicle); the others drive their copy from that.
 *
 * Wire contract:
 *   construction -> model (actor definition index)
 *   transform    -> position, heading, speed, the rider's input to it (unreliable, every tick, owner-authored)
 *   fields       -> occupant, health (server fields)
 */

#pragma once

#include <networking/replication/network_entity.h>

#include <cstdint>

namespace RDMP::Shared {
    using Framework::Networking::Replication::FieldSerializer;

    struct Vehicle final: Framework::Networking::Replication::NetworkEntity {
        static constexpr const char *kTypeName = "RDMP::Vehicle";
        static constexpr float kStreamRange    = 250.0f;
        static constexpr float kWorldMin       = -10000.0f;
        static constexpr float kWorldMax       = 10000.0f;
        static constexpr int kPosBits          = 22;
        static constexpr uint32_t kModelMin    = 976;  // RIDEABLE_ANIMAL_Horse01
        static constexpr uint32_t kModelMax    = 1005; // RIDEABLE_ANIMAL_Buffalo04
        static constexpr uint32_t kDefaultModel = 995; // RIDEABLE_ANIMAL_SaddleHorse01

        // --- construction ---
        uint32_t model = kDefaultModel;

        // --- transform (owner = rider, else the server) ---
        float heading     = 0.0f; // degrees
        float speed       = 0.0f; // m/s
        float moveRight   = 0.0f; // the input the rider gives it (shared/types.h MoveInput: its locomotion desire)
        float moveBack    = 0.0f;
        float moveHeading = 0.0f;
        uint8_t gait      = 0;

        // --- server fields ---
        uint64_t occupant = 0; // network id of the player riding it, 0 = none
        float health      = 1000.0f;

        void OnSerializeConstruction(FieldSerializer &fields) override {
            fields.Field(model);
        }

        void SerializeTransform(FieldSerializer &fields) override {
            fields.FloatBits(position.x, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.y, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.z, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(heading, -180.0f, 180.0f, 16);
            fields.FloatBits(speed, 0.0f, 60.0f, 8);
            fields.FloatBits(moveRight, -1.0f, 1.0f, 8);
            fields.FloatBits(moveBack, -1.0f, 1.0f, 8);
            fields.FloatBits(moveHeading, 0.0f, 6.2831853f, 12);
            fields.BitsRange<uint8_t>(gait, 0, 255);
        }

        void SerializeFields(FieldSerializer &fields) override {
            fields.ServerField(occupant);
            fields.ServerField(health);
        }

        void SerializeForcedState(FieldSerializer &fields) override {
            fields.Field(occupant);
            fields.Field(health);
        }
    };
} // namespace RDMP::Shared
