/*
 * SERVER element functions (players and blips are elements):
 *   getElementPosition(element) -> {x, y, z}            setElementPosition(element, x, y, z)
 *   getElementType(element) -> "player" | "blip" | false  isElement(value)
 *   getElementsByType("player" | "blip") -> [elements]
 *   destroyElement(element)                             (blips; players leave by disconnecting)
 *   attachElements(blip, element) / detachElements(blip) / getElementAttachedTo(blip) -> element | null
 *   setElementVisibleTo(blip, player | null, visible)   MTA semantics: a player = for them; null = everyone / nobody
 */

#include "server/server.h"


#include <unordered_set>
#include <vector>

namespace RDMP::Server {
    namespace {
        using namespace ScriptApi;

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

    // ---- generic element natives (players + blips) --------------------------------------------
    //
    //   getElementPosition(element) -> {x, y, z}       setElementPosition(element, x, y, z)
    //   getElementType(element) -> "player"|"blip"|false   isElement(element)
    //   getElementsByType("player"|"blip") -> [elements]
    //   destroyElement(blip)                            attachElements(blip, target) / detachElements(blip)
    //   getElementAttachedTo(blip) -> element|null      setElementVisibleTo(blip, player|null)
    void GetPositionCallback(const Args &args) {
        auto *isolate = args.GetIsolate();
        glm::vec3 p {};
        const uint64_t id = ElementId(args, 0);
        if (!id || !ElementPosition(GetWorld(), id, p)) {
            Throw(isolate, "getElementPosition(element) -> {x,y,z}");
            return;
        }
        auto context = isolate->GetCurrentContext();
        auto result  = v8::Object::New(isolate);
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "x"), v8::Number::New(isolate, p.x)).Check();
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "y"), v8::Number::New(isolate, p.y)).Check();
        result->Set(context, v8::String::NewFromUtf8Literal(isolate, "z"), v8::Number::New(isolate, p.z)).Check();
        args.GetReturnValue().Set(result);
    }

    void SetPositionCallback(const Args &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        if (!id || args.Length() < 4) {
            Throw(isolate, "setElementPosition(element, x, y, z)");
            return;
        }
        const float x = static_cast<float>(Num(args, 1, 0.0));
        const float y = static_cast<float>(Num(args, 2, 0.0));
        const float z = static_cast<float>(Num(args, 3, 0.0));
        auto &world   = GetWorld();
        if (auto player = world.Write<Player>(id)) {
            Teleport(player, x, y, z); // the client sees a server-authored pose and moves the real player
            args.GetReturnValue().Set(true);
            return;
        }
        if (auto blip = world.Write<Blip>(id)) {
            blip->position = {x, y, z}; // an attached blip keeps following its element (MTA does the same)
            args.GetReturnValue().Set(true);
            return;
        }
        if (auto vehicle = world.Write<RDMP::Shared::Vehicle>(id)) {
            vehicle->position = {x, y, z};
            vehicle->speed    = 0.0f;
            vehicle->ForceState(); // a ridden one: its rider's client takes the server's pose
            args.GetReturnValue().Set(true);
            return;
        }
        args.GetReturnValue().Set(false);
    }

    void GetTypeCallback(const Args &args) {
        auto *isolate     = args.GetIsolate();
        const uint64_t id = ElementId(args, 0);
        auto &world       = GetWorld();
        if (id && world.Is<Player>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "player"));
        }
        else if (id && world.Is<Blip>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "blip"));
        }
        else if (id && world.Is<RDMP::Shared::Vehicle>(id)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, "vehicle"));
        }
        else {
            args.GetReturnValue().Set(false);
        }
    }

    void IsElementCallback(const Args &args) {
        const uint64_t id = ElementId(args, 0);
        args.GetReturnValue().Set(id != 0 && GetWorld().Exists(id));
    }

    void GetElementsByTypeCallback(const Args &args) {
        auto *isolate = args.GetIsolate();
        if (!Has(args, 0) || !args[0]->IsString()) {
            Throw(isolate, "getElementsByType(\"player\"|\"blip\"|\"vehicle\") -> [elements]");
            return;
        }
        const std::string type = *v8::String::Utf8Value(isolate, args[0]);
        std::vector<uint64_t> ids;
        if (type == "player") {
            ids = GetWorld().IdsOf<Player>();
        }
        else if (type == "blip") {
            ids = GetWorld().IdsOf<Blip>();
        }
        else if (type == "vehicle") {
            ids = GetWorld().IdsOf<RDMP::Shared::Vehicle>();
        }
        auto context = isolate->GetCurrentContext();
        auto result  = v8::Array::New(isolate, static_cast<int>(ids.size()));
        for (uint32_t i = 0; i < ids.size(); ++i) {
            result->Set(context, i, WrapElement(isolate, ids[i])).Check();
        }
        args.GetReturnValue().Set(result);
    }


    void DestroyElementCallback(const Args &args) {
        const uint64_t id = ElementId(args, 0);
        auto &world       = GetWorld();
        if (!id || !(world.Is<Blip>(id) || world.Is<RDMP::Shared::Vehicle>(id))) {
            args.GetReturnValue().Set(false); // players are not destroyable
            return;
        }
        const bool destroyed = world.Destroy(id);
        if (destroyed) {
            ForgetElement(id);
        }
        args.GetReturnValue().Set(destroyed);
    }

    void AttachElementsCallback(const Args &args) {
        const uint64_t target = ElementId(args, 1);
        glm::vec3 pos {};
        if (!target || target == ElementId(args, 0) || !ElementPosition(GetWorld(), target, pos)) {
            Throw(args.GetIsolate(), "attachElements(blip, element)");
            return;
        }
        WithBlip(args, "attachElements(blip, element)", [&](Ref<Blip> &b) {
            b->attachedTo = target;
            b->position   = pos;
            args.GetReturnValue().Set(true);
        });
    }
    void DetachElementsCallback(const Args &args) {
        WithBlip(args, "detachElements(blip)", [&](Ref<Blip> &b) {
            const bool was = b->attachedTo != 0;
            b->attachedTo  = 0;
            args.GetReturnValue().Set(was);
        });
    }
    void GetAttachedToCallback(const Args &args) {
        uint64_t target = 0;
        bool isBlip     = false;
        WithBlip(args, "getElementAttachedTo(blip)", [&](Ref<Blip> &b) {
            target = b->attachedTo;
            isBlip = true;
        });
        if (!isBlip) {
            return; // WithBlip threw
        }
        args.GetReturnValue().Set(WrapElement(args.GetIsolate(), target && GetWorld().Exists(target) ? target : 0));
    }
    // setElementVisibleTo(blip, player, visible): MTA semantics. A player: let them see it (true) or not (false).
    // null: everyone (true) or nobody (false).
    void SetVisibleToCallback(const Args &args) {
        auto *isolate     = args.GetIsolate();
        const char *usage = "setElementVisibleTo(blip, player|null, visible)";
        const uint64_t id = ElementId(args, 0);
        if (!id || !GetWorld().Is<Blip>(id)) {
            Throw(isolate, usage);
            return;
        }
        const bool visible            = args.Length() > 2 ? args[2]->BooleanValue(isolate) : true;
        const uint64_t player         = ElementId(args, 1);
        const MafiaNet::PeerGuid guid = player ? PlayerGuid(player) : MafiaNet::UNASSIGNED_PEER_GUID;
        if (player && guid == MafiaNet::UNASSIGNED_PEER_GUID) {
            Throw(isolate, "setElementVisibleTo: not a connected player");
            return;
        }
        auto blip = GetWorld().Write<Blip>(id);
        if (!blip) {
            Throw(isolate, usage);
            return;
        }
        if (player) {
            Replication()->SetViewerAdmitted(*blip.operator->(), guid, visible);
        }
        else {
            Framework::Networking::Replication::ViewerFilter filter;
            if (!visible) {
                filter.only = std::unordered_set<MafiaNet::PeerGuid> {};
            }
            Replication()->SetViewerFilter(*blip.operator->(), std::move(filter));
            ForgetVisibleOnly(id);
        }
        args.GetReturnValue().Set(true);
    }
    } // namespace

    void RegisterElementApi(const ScriptApi::Publisher &publish) {
        publish("getElementPosition", &GetPositionCallback);
        publish("setElementPosition", &SetPositionCallback);
        publish("getElementType", &GetTypeCallback);
        publish("isElement", &IsElementCallback);
        publish("getElementsByType", &GetElementsByTypeCallback);
        publish("destroyElement", &DestroyElementCallback);
        publish("attachElements", &AttachElementsCallback);
        publish("detachElements", &DetachElementsCallback);
        publish("getElementAttachedTo", &GetAttachedToCallback);
        publish("setElementVisibleTo", &SetVisibleToCallback);
    }
} // namespace RDMP::Server
