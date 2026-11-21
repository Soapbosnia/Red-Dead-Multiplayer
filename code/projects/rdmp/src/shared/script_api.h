#pragma once
/*
 * Script API plumbing used by both sides (RDMPServer.exe and rdmp_client.dll both embed V8): argument helpers, the
 * Publisher every API module registers its functions through, and the SHARED functions -- the ones that exist with the
 * same name and meaning on the server and the client:
 *
 *   tocolor(r, g, b[, a = 255]) -> 0xAARRGGBB      (MTA's tocolor; dx* drawing and nametag colours take it)
 *   BlipIcon.*                                     RDR blip icon ids by name
 *
 * Side-specific functions live in src/server/api_*.cpp and src/client/api_*.cpp; docs/SCRIPTING.md lists all of them.
 */

#include <v8.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "shared/blip_icons.h"

namespace RDMP::ScriptApi {
    using Args = v8::FunctionCallbackInfo<v8::Value>;

    // Publishes one global function into the resource context being set up.
    using Publisher = std::function<void(const char *name, v8::FunctionCallback callback)>;

    inline void Throw(v8::Isolate *isolate, const std::string &usage) {
        isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8(isolate, usage.c_str()).ToLocalChecked()));
    }

    inline bool Has(const Args &args, int i) {
        return args.Length() > i && !args[i]->IsUndefined() && !args[i]->IsNull();
    }

    inline double Num(const Args &args, int i, double fallback) {
        if (!Has(args, i) || !args[i]->IsNumber()) {
            return fallback;
        }
        return args[i].As<v8::Number>()->Value();
    }

    inline float Float(const Args &args, int i, float fallback) {
        return static_cast<float>(Num(args, i, fallback));
    }

    inline bool Bool(const Args &args, int i, bool fallback) {
        return Has(args, i) ? args[i]->BooleanValue(args.GetIsolate()) : fallback;
    }

    inline std::string Str(v8::Isolate *isolate, v8::Local<v8::Value> v) {
        return *v8::String::Utf8Value(isolate, v);
    }

    inline std::string Str(const Args &args, int i, const std::string &fallback = {}) {
        return Has(args, i) ? Str(args.GetIsolate(), args[i]) : fallback;
    }

    inline v8::Local<v8::String> JsString(v8::Isolate *isolate, const std::string &s) {
        return v8::String::NewFromUtf8(isolate, s.c_str()).ToLocalChecked();
    }

    inline v8::Local<v8::Object> Vec3(v8::Isolate *isolate, float x, float y, float z) {
        auto context = isolate->GetCurrentContext();
        auto o       = v8::Object::New(isolate);
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "x"), v8::Number::New(isolate, x)).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "y"), v8::Number::New(isolate, y)).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "z"), v8::Number::New(isolate, z)).Check();
        return o;
    }

    inline v8::Local<v8::Object> Vec2(v8::Isolate *isolate, float x, float y) {
        auto context = isolate->GetCurrentContext();
        auto o       = v8::Object::New(isolate);
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "x"), v8::Number::New(isolate, x)).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "y"), v8::Number::New(isolate, y)).Check();
        return o;
    }

    // {x, y, z} or [x, y, z].
    inline bool ReadVec3(v8::Isolate *isolate, v8::Local<v8::Value> v, std::array<float, 3> &out) {
        if (!v->IsObject()) {
            return false;
        }
        auto context        = isolate->GetCurrentContext();
        auto o              = v.As<v8::Object>();
        const char *keys[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            v8::Local<v8::Value> c;
            const bool ok = v->IsArray() ? o->Get(context, static_cast<uint32_t>(i)).ToLocal(&c)
                                         : o->Get(context, v8::String::NewFromUtf8(isolate, keys[i]).ToLocalChecked()).ToLocal(&c);
            if (!ok) {
                return false;
            }
            out[i] = c->IsNumber() ? static_cast<float>(c.As<v8::Number>()->Value()) : 0.0f;
        }
        return true;
    }

    // A colour argument: a tocolor() number (0xAARRGGBB); `fallback` when missing.
    inline uint32_t Color(const Args &args, int i, uint32_t fallback) {
        if (!Has(args, i) || !args[i]->IsNumber()) {
            return fallback;
        }
        return static_cast<uint32_t>(static_cast<int64_t>(args[i].As<v8::Number>()->Value()) & 0xFFFFFFFFll);
    }

    inline uint32_t ToColor(double r, double g, double b, double a) {
        const auto byte = [](double v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0, 255.0));
        };
        return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
    }

    // ---- SHARED functions ----
    inline void ToColorFn(const Args &args) {
        args.GetReturnValue().Set(static_cast<double>(ToColor(Num(args, 0, 255), Num(args, 1, 255), Num(args, 2, 255), Num(args, 3, 255))));
    }

    inline void RegisterShared(v8::Isolate *isolate, v8::Local<v8::Context> context, const Publisher &publish) {
        publish("tocolor", &ToColorFn);
        auto icons = v8::Object::New(isolate);
        for (const auto &icon : RDMP::Shared::kBlipIcons) {
            icons->Set(context, v8::String::NewFromUtf8(isolate, icon.name).ToLocalChecked(), v8::Integer::New(isolate, icon.id)).Check();
        }
        context->Global()->Set(context, v8::String::NewFromUtf8Literal(isolate, "BlipIcon"), icons).Check();
    }

    // The publisher for a resource context's global object.
    inline Publisher GlobalPublisher(v8::Isolate *isolate, v8::Local<v8::Context> context) {
        return [isolate, context](const char *name, v8::FunctionCallback callback) {
            auto fn = v8::Function::New(context, callback).ToLocalChecked();
            context->Global()->Set(context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(), fn).Check();
        };
    }
} // namespace RDMP::ScriptApi
