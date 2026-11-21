/*
 * RDMP - script blip entity (server-owned, replicated to every client or to one player).
 *
 * The server creates one per createBlip / createBlipAttachedTo call. It is server-owned, so it replicates to
 * everyone it streams to with no ForceState; streaming.alwaysVisible makes it map-wide (a blip far away must
 * still be on the pause map), and streaming.targetGUID (Entity::SetVisibleTo) restricts it to one player.
 * The game side (rdmp.dll, core/blips.cpp) turns the set into engine blips.
 *
 * Wire contract:
 *   transform -> position (the blip's own, or the attached element's position copied by the server ~10 Hz)
 *   fields    -> icon, colour, scale, max distance, priority, flags, attached element (reliable deltas)
 */

#pragma once

#include <networking/replication/network_entity.h>

#include <cstdint>

namespace RDMP::Shared {
    using Framework::Networking::Replication::FieldSerializer;

    struct Blip final: Framework::Networking::Replication::NetworkEntity {
        static constexpr const char *kTypeName = "RDMP::Blip";
        static constexpr float kWorldMin       = -10000.0f;
        static constexpr float kWorldMax       = 10000.0f;
        static constexpr int kPosBits          = 20; // ~2 cm: plenty for a map icon

        // Same bits as rdmp::fw::BlipFlags (src/shared/fw_api.h).
        enum Flags : uint8_t {
            kHidden    = 1u << 0,
            kRadarOnly = 1u << 1,
            kMapOnly   = 1u << 2,
            kHasColor  = 1u << 3,
        };

        static constexpr uint16_t kIconMin     = 299; // first BlipIcon (FUN_140606C60 special-cases 299..566)
        static constexpr uint16_t kIconMax     = 572; // last BlipIcon in the native DB (BLIP_NORTHARROW)
        static constexpr uint16_t kDefaultIcon = 330; // BLIP_OBJECTIVE

        uint16_t icon       = kDefaultIcon;
        uint32_t rgba       = 0xFFFFFFFFu; // 0xRRGGBBAA, used only with kHasColor
        float scale         = 1.0f;
        float maxDistance   = -1.0f;       // < 0 = unlimited
        int16_t priority    = 0;           // MTA "ordering"
        uint8_t flags       = 0;
        uint64_t attachedTo = 0;           // network id of the followed element, 0 = none

        void SerializeTransform(FieldSerializer &fields) override {
            fields.FloatBits(position.x, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.y, kWorldMin, kWorldMax, kPosBits);
            fields.FloatBits(position.z, kWorldMin, kWorldMax, kPosBits);
        }

        void SerializeFields(FieldSerializer &fields) override {
            fields.Field(icon);
            fields.Field(rgba);
            fields.Field(scale);
            fields.Field(maxDistance);
            fields.Field(priority);
            fields.Field(flags);
            fields.Field(attachedTo);
        }
    };
} // namespace RDMP::Shared
