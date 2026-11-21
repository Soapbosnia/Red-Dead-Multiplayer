/*
 * SERVER radar blips (MTA argument order):
 *   createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
 *   createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
 *   get/setBlipIcon, get/setBlipSize, get/setBlipColor, get/setBlipOrdering, get/setBlipVisibleDistance,
 *   setBlipVisible(blip, bool), setBlipDisplay(blip, "both" | "radar" | "map")
 */

#include "server/server.h"


#include <algorithm>
#include <unordered_set>
#include <vector>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;

        constexpr size_t kMaxBlips     = 1024;                          // server-wide; a client shows at most 120
        constexpr auto kAttachInterval = std::chrono::milliseconds(100); // attached blips follow their element at 10 Hz

    // Runs `fn` on the blip named by args[0]; throws `usage` if it is not a blip.
    template <typename Fn>
    void WithBlip(const Args &args, const char *usage, Fn &&fn) {
        const uint64_t id = ElementId(args, 0);
        auto blip         = id ? GetWorld().Write<Blip>(id) : Ref<Blip>();
        if (!blip) {
            Throw(args.GetIsolate(), usage);
            return;
        }
        fn(blip);
    }

    // ---- blips ------------------------------------------------------------------------------------
    //
    //   createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
    //   createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo]) -> blip
    //   get/setBlipIcon, get/setBlipSize, get/setBlipColor, get/setBlipOrdering, get/setBlipVisibleDistance,
    //   setBlipVisible(blip, bool), setBlipDisplay(blip, "both"|"radar"|"map")
    //
    // MTA argument order. Differences: icon is an RDR BlipIcon (number 299..572 or a name such as
    // "BLIP_OBJECTIVE" / "objective"; BlipIcon.* holds them all); size maps to the engine scale as size / 2
    // (MTA's default 2 = scale 1); the colour is a tint applied only when given (RDR icons carry their own
    // colours); visibleDistance >= 16383 (MTA's default) means unlimited.
    uint16_t IconArg(const Args &args, int i, uint16_t fallback) {
        if (!Has(args, i)) {
            return fallback;
        }
        if (args[i]->IsString()) {
            const std::string name = *v8::String::Utf8Value(args.GetIsolate(), args[i]);
            const uint16_t id      = RDMP::Shared::BlipIconFromName(name.c_str());
            return id ? id : fallback;
        }
        const double v = Num(args, i, fallback);
        return (v >= Blip::kIconMin && v <= Blip::kIconMax) ? static_cast<uint16_t>(v) : fallback;
    }

    uint8_t ColorByte(const Args &args, int i, double fallback) {
        return static_cast<uint8_t>(std::clamp(Num(args, i, fallback), 0.0, 255.0));
    }

    float DistanceArg(double v) {
        return (v <= 0.0 || v >= kUnlimitedDistance) ? -1.0f : static_cast<float>(v);
    }

    // Shared tail of createBlip / createBlipAttachedTo: args[first..] = icon, size, r, g, b, a, ordering,
    // visibleDistance, visibleTo.
    void CreateBlipCommon(const Args &args, int first, const glm::vec3 &pos, uint64_t attachedTo) {
        auto *isolate = args.GetIsolate();
        auto &world   = GetWorld();
        if (world.IdsOf<Blip>().size() >= kMaxBlips) {
            Log()->warn("createBlip: the server already has {} blips", kMaxBlips);
            args.GetReturnValue().Set(false);
            return;
        }
        const MafiaNet::PeerGuid visibleTo = PlayerGuid(ElementId(args, first + 8));
        const std::string resource         = CallingResource(isolate);
        uint64_t id                        = 0;
        {
            auto blip = world.Create<Blip>();
            if (!blip) {
                args.GetReturnValue().Set(false);
                return;
            }
            blip->position    = pos;
            blip->icon        = IconArg(args, first, Blip::kDefaultIcon);
            blip->scale       = static_cast<float>(std::clamp(Num(args, first + 1, 2.0), 0.0, 50.0) / 2.0);
            const bool colour = Has(args, first + 2) || Has(args, first + 3) || Has(args, first + 4) || Has(args, first + 5);
            if (colour) {
                blip->rgba  = (uint32_t(ColorByte(args, first + 2, 255)) << 24) | (uint32_t(ColorByte(args, first + 3, 255)) << 16) |
                             (uint32_t(ColorByte(args, first + 4, 255)) << 8) | ColorByte(args, first + 5, 255);
                blip->flags = static_cast<uint8_t>(blip->flags | Blip::kHasColor);
            }
            blip->priority             = static_cast<int16_t>(std::clamp(Num(args, first + 6, 0.0), -32768.0, 32767.0));
            blip->maxDistance          = DistanceArg(Num(args, first + 7, kUnlimitedDistance));
            blip->attachedTo           = attachedTo;
            blip->streaming.alwaysVisible = true; // map-wide, like the story's blips
            id                         = blip.Id();
            if (visibleTo != MafiaNet::UNASSIGNED_PEER_GUID) {
                Framework::Networking::Replication::ViewerFilter filter;
                filter.only = std::unordered_set<MafiaNet::PeerGuid> {visibleTo};
                Replication()->SetViewerFilter(*blip.operator->(), std::move(filter));
            }
        }
        OwnElement(id, resource, visibleTo);
        args.GetReturnValue().Set(Instance()->WrapScriptEntity(isolate, id));
    }

    void CreateBlipCallback(const Args &args) {
        if (args.Length() < 3 || !args[0]->IsNumber() || !args[1]->IsNumber() || !args[2]->IsNumber()) {
            Throw(args.GetIsolate(), "createBlip(x, y, z[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])");
            return;
        }
        const glm::vec3 pos {static_cast<float>(Num(args, 0, 0)), static_cast<float>(Num(args, 1, 0)), static_cast<float>(Num(args, 2, 0))};
        CreateBlipCommon(args, 3, pos, 0);
    }

    void CreateBlipAttachedToCallback(const Args &args) {
        const uint64_t target = ElementId(args, 0);
        glm::vec3 pos {};
        if (!target || !ElementPosition(GetWorld(), target, pos)) {
            Throw(args.GetIsolate(), "createBlipAttachedTo(element[, icon, size, r, g, b, a, ordering, visibleDistance, visibleTo])");
            return;
        }
        CreateBlipCommon(args, 1, pos, target);
    }

    void SetBlipIconCallback(const Args &args) {
        WithBlip(args, "setBlipIcon(blip, icon)", [&](Ref<Blip> &b) {
            const uint16_t icon = IconArg(args, 1, 0);
            if (icon) {
                b->icon = icon;
            }
            args.GetReturnValue().Set(icon != 0);
        });
    }
    void GetBlipIconCallback(const Args &args) {
        WithBlip(args, "getBlipIcon(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<uint32_t>(b->icon)); });
    }
    void SetBlipSizeCallback(const Args &args) {
        WithBlip(args, "setBlipSize(blip, size)", [&](Ref<Blip> &b) {
            b->scale = static_cast<float>(std::clamp(Num(args, 1, 2.0), 0.0, 50.0) / 2.0);
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipSizeCallback(const Args &args) {
        WithBlip(args, "getBlipSize(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<double>(b->scale) * 2.0); });
    }
    void SetBlipColorCallback(const Args &args) {
        WithBlip(args, "setBlipColor(blip, r, g, b[, a])", [&](Ref<Blip> &b) {
            b->rgba  = (uint32_t(ColorByte(args, 1, 255)) << 24) | (uint32_t(ColorByte(args, 2, 255)) << 16) |
                      (uint32_t(ColorByte(args, 3, 255)) << 8) | ColorByte(args, 4, 255);
            b->flags = static_cast<uint8_t>(b->flags | Blip::kHasColor);
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipColorCallback(const Args &args) {
        WithBlip(args, "getBlipColor(blip) -> [r, g, b, a]", [&](Ref<Blip> &b) {
            auto *isolate = args.GetIsolate();
            auto context  = isolate->GetCurrentContext();
            auto result   = v8::Array::New(isolate, 4);
            for (uint32_t i = 0; i < 4; ++i) {
                result->Set(context, i, v8::Integer::New(isolate, static_cast<int>((b->rgba >> (24 - 8 * i)) & 0xff))).Check();
            }
            args.GetReturnValue().Set(result);
        });
    }
    void SetBlipOrderingCallback(const Args &args) {
        WithBlip(args, "setBlipOrdering(blip, ordering)", [&](Ref<Blip> &b) {
            b->priority = static_cast<int16_t>(std::clamp(Num(args, 1, 0.0), -32768.0, 32767.0));
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipOrderingCallback(const Args &args) {
        WithBlip(args, "getBlipOrdering(blip)", [&](Ref<Blip> &b) { args.GetReturnValue().Set(static_cast<int>(b->priority)); });
    }
    void SetBlipVisibleDistanceCallback(const Args &args) {
        WithBlip(args, "setBlipVisibleDistance(blip, distance)", [&](Ref<Blip> &b) {
            b->maxDistance = DistanceArg(Num(args, 1, kUnlimitedDistance));
            args.GetReturnValue().Set(true);
        });
    }
    void GetBlipVisibleDistanceCallback(const Args &args) {
        WithBlip(args, "getBlipVisibleDistance(blip)", [&](Ref<Blip> &b) {
            args.GetReturnValue().Set(b->maxDistance < 0.0f ? static_cast<double>(kUnlimitedDistance) : static_cast<double>(b->maxDistance));
        });
    }
    void SetBlipVisibleCallback(const Args &args) {
        WithBlip(args, "setBlipVisible(blip, visible)", [&](Ref<Blip> &b) {
            const bool visible = args.Length() > 1 ? args[1]->BooleanValue(args.GetIsolate()) : true;
            b->flags           = static_cast<uint8_t>(visible ? (b->flags & ~Blip::kHidden) : (b->flags | Blip::kHidden));
            args.GetReturnValue().Set(true);
        });
    }
    void SetBlipDisplayCallback(const Args &args) {
        WithBlip(args, "setBlipDisplay(blip, \"both\"|\"radar\"|\"map\")", [&](Ref<Blip> &b) {
            const std::string mode = Has(args, 1) ? *v8::String::Utf8Value(args.GetIsolate(), args[1]) : "both";
            uint8_t flags          = static_cast<uint8_t>(b->flags & ~(Blip::kRadarOnly | Blip::kMapOnly));
            if (mode == "radar") {
                flags |= Blip::kRadarOnly;
            }
            else if (mode == "map") {
                flags |= Blip::kMapOnly;
            }
            else if (mode != "both") {
                args.GetReturnValue().Set(false);
                return;
            }
            b->flags = flags;
            args.GetReturnValue().Set(true);
        });
    }

    } // namespace

    void RegisterBlipApi(const ScriptApi::Publisher &publish) {
        publish("createBlip", &CreateBlipCallback);
        publish("createBlipAttachedTo", &CreateBlipAttachedToCallback);
        publish("setBlipIcon", &SetBlipIconCallback);
        publish("getBlipIcon", &GetBlipIconCallback);
        publish("setBlipSize", &SetBlipSizeCallback);
        publish("getBlipSize", &GetBlipSizeCallback);
        publish("setBlipColor", &SetBlipColorCallback);
        publish("getBlipColor", &GetBlipColorCallback);
        publish("setBlipOrdering", &SetBlipOrderingCallback);
        publish("getBlipOrdering", &GetBlipOrderingCallback);
        publish("setBlipVisibleDistance", &SetBlipVisibleDistanceCallback);
        publish("getBlipVisibleDistance", &GetBlipVisibleDistanceCallback);
        publish("setBlipVisible", &SetBlipVisibleCallback);
        publish("setBlipDisplay", &SetBlipDisplayCallback);
    }

    // Tick thread: attached blips follow their element; a destroyed target leaves the blip where it was
    // (MTA detaches on destroy).
    void UpdateAttachedBlips(World &world) {
        using namespace std::chrono_literals;
        static Clock::time_point next {};
        const auto now = Clock::now();
        if (now < next) {
            return;
        }
        next = now + kAttachInterval;
        std::vector<std::pair<uint64_t, uint64_t>> attached;
        world.ForEachRead<Blip>([&](const auto &blip) {
            if (blip->attachedTo) {
                attached.emplace_back(blip.Id(), blip->attachedTo);
            }
        });
        for (const auto &[blipId, target] : attached) {
            glm::vec3 pos {};
            const bool found = ElementPosition(world, target, pos);
            if (auto blip = world.Write<Blip>(blipId)) {
                if (!found) {
                    blip->attachedTo = 0;
                }
                else if (glm::distance(blip->position, pos) > 0.25f) {
                    blip->position = pos;
                }
            }
        }
    }
} // namespace RDMP::Server
