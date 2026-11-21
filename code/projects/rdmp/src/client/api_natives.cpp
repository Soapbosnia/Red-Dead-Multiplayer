/*
 * CLIENT game natives:
 *   invokeNative(nameOrHash, types, ...args)  any game native, e.g. invokeNative("SET_WEATHER", "if", 3, 1.0)
 *       types: i int, u uint, f float, b bool, s string, v Vector3 in ({x,y,z} or [x,y,z]), V Vector3 out;
 *       ">x" at the end sets the return type (i, u, f, b, n = none; default i). With V args the result is
 *       {result, vectors: [{x,y,z}, ...]}. Names are hashed with joaat (lower case); re/natives_0x673783F3.csv lists them.
 */

#include <utils/safe_win32.h>

#include "client/api.h"

#include <array>
#include <cctype>
#include <cstring>

namespace RDMP::ClientApi {
    namespace {
        using namespace ScriptApi;

    uint32_t Joaat(const std::string &name) {
        uint32_t h = 0;
        for (char ch : name) {
            h += static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(ch)));
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }

    void InvokeNativeFn(const Args &args) {
        auto *isolate      = args.GetIsolate();
        const char *usage  = "invokeNative(nameOrHash, types, ...args): types i u f b s v V, \">x\" return type";
        if (args.Length() < 2 || !args[1]->IsString() || !Host()) {
            Throw(isolate, usage);
            return;
        }
        uint32_t hash = 0;
        if (args[0]->IsString()) {
            hash = Joaat(Str(isolate, args[0]));
        }
        else if (args[0]->IsNumber()) {
            hash = static_cast<uint32_t>(args[0].As<v8::Number>()->Value());
        }
        else {
            Throw(isolate, usage);
            return;
        }
        const std::string spec = Str(isolate, args[1]);
        const size_t arrow     = spec.find('>');
        const std::string in   = spec.substr(0, arrow);
        const char ret         = arrow != std::string::npos && arrow + 1 < spec.size() ? spec[arrow + 1] : 'i';
        if (static_cast<int>(in.size()) != args.Length() - 2 || in.size() > 24) {
            Throw(isolate, std::string(usage) + " (types and arguments differ in count)");
            return;
        }
        uint64_t slots[24] = {};
        std::vector<std::string> strings;
        strings.reserve(in.size());
        std::vector<std::array<float, 3>> vectors;
        vectors.reserve(in.size());
        std::vector<size_t> outVectors;
        for (size_t i = 0; i < in.size(); ++i) {
            v8::Local<v8::Value> v = args[static_cast<int>(i) + 2];
            uint32_t bits          = 0;
            switch (in[i]) {
            case 'i': bits = static_cast<uint32_t>(static_cast<int32_t>(v->IsNumber() ? v.As<v8::Number>()->Value() : 0)); slots[i] = bits; break;
            case 'u': slots[i] = static_cast<uint32_t>(v->IsNumber() ? v.As<v8::Number>()->Value() : 0); break;
            case 'b': slots[i] = v->BooleanValue(isolate) ? 1u : 0u; break;
            case 'f': {
                const float f = v->IsNumber() ? static_cast<float>(v.As<v8::Number>()->Value()) : 0.0f;
                std::memcpy(&bits, &f, 4);
                slots[i] = bits;
                break;
            }
            case 's':
                strings.push_back(Str(isolate, v));
                slots[i] = reinterpret_cast<uint64_t>(strings.back().c_str());
                break;
            case 'v':
            case 'V': {
                vectors.push_back({0.0f, 0.0f, 0.0f});
                if (in[i] == 'v' && !ReadVec3(isolate, v, vectors.back())) {
                    Throw(isolate, "invokeNative: a 'v' argument must be {x, y, z} or [x, y, z]");
                    return;
                }
                if (in[i] == 'V') {
                    outVectors.push_back(vectors.size() - 1);
                }
                slots[i] = reinterpret_cast<uint64_t>(vectors.back().data());
                break;
            }
            default: Throw(isolate, usage); return;
            }
        }
        uint64_t results[rdmp::fw::kResultSlots] = {};
        if (!Host()->invoke_native(hash, slots, static_cast<int>(in.size()), results)) {
            Throw(isolate, "invokeNative: native not registered (wrong name?)");
            return;
        }
        v8::Local<v8::Value> value;
        const uint32_t low = static_cast<uint32_t>(results[0]);
        switch (ret) {
        case 'u': value = v8::Number::New(isolate, low); break;
        case 'b': value = v8::Boolean::New(isolate, (low & 0xff) != 0); break;
        case 'f': {
            float f = 0.0f;
            std::memcpy(&f, &low, 4);
            value = v8::Number::New(isolate, f);
            break;
        }
        case 'n': value = v8::Undefined(isolate); break;
        default: value = v8::Integer::New(isolate, static_cast<int32_t>(low)); break;
        }
        if (outVectors.empty()) {
            args.GetReturnValue().Set(value);
            return;
        }
        auto context = isolate->GetCurrentContext();
        auto list    = v8::Array::New(isolate, static_cast<int>(outVectors.size()));
        for (uint32_t i = 0; i < outVectors.size(); ++i) {
            const auto &v = vectors[outVectors[i]];
            list->Set(context, i, Vec3(isolate, v[0], v[1], v[2])).Check();
        }
        auto o = v8::Object::New(isolate);
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "result"), value).Check();
        o->Set(context, v8::String::NewFromUtf8Literal(isolate, "vectors"), list).Check();
        args.GetReturnValue().Set(o);
    }

    } // namespace

    void RegisterNativeApi(const ScriptApi::Publisher &publish) {
        publish("invokeNative", &InvokeNativeFn);
    }
} // namespace RDMP::ClientApi
