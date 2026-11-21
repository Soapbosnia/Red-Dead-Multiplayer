#pragma once
// Calling the game's own script natives by name hash (RDR1 PC: 32-bit joaat of the lowercase native name).
//
// RE (docs/SCRIPTING_RE.md, "Native registry"): every native is registered at boot by
//   lea r8,[handler] ; mov edx,<hash> ; call 0x1401E7140
// and 0x1401E7140 inserts {hash, handler} into an open-addressed table:
//   *(0x142B0D210) = entries (16 bytes: u32 hash, pad, u64 handler), *(u32*)0x142B0D218 = capacity,
//   *(u32*)0x142B0D220 = count; slot = hash % capacity, probe step = (hash >> 1) + 1, key <= 1 = free.
// ResolveNative() reads that table exactly like the engine. Names/hashes: K3rhos' RDR-PC-Natives-DB (MIT).
//
// Natives touch live game state: call Invoke() on the game's main thread only (RunOnMainThread /
// AddFrameCallback).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdmp {

// Hashes of the natives the mod calls (joaat(lowercase name)), build 0x673783F3.
namespace nat {
constexpr uint32_t ADD_BLIP_FOR_COORD     = 0xC6F43D0E;  // (Vector3* pos, int icon, float minDist, bool, int) -> Blip
constexpr uint32_t ADD_BLIP_FOR_ACTOR     = 0xEFB9362F;  // (Actor, int icon, float minDist, bool, int) -> Blip
constexpr uint32_t REMOVE_BLIP            = 0xD8C3C1CD;  // (Blip)
constexpr uint32_t IS_BLIP_VALID          = 0xDCC10BA9;  // (Blip) -> bool
constexpr uint32_t CHANGE_BLIP_ICON       = 0xABD125F6;  // (Blip, int icon)
constexpr uint32_t GET_BLIP_ICON          = 0xEE4F4B7D;  // (Blip) -> int
constexpr uint32_t SET_BLIP_POS           = 0xB2EAF8DD;  // (Blip, Vector3*)
constexpr uint32_t SET_BLIP_SCALE         = 0x1E6EC434;  // (Blip, float)
constexpr uint32_t SET_BLIP_COLOR         = 0xA2B8A736;  // (Blip, float r, g, b, a)   0..1 each (engine * 255)
constexpr uint32_t SET_BLIP_PRIORITY      = 0xCE87DA6F;  // (Blip, int)
constexpr uint32_t SET_BLIP_VISIBLE       = 0x9318D3D2;  // (Blip, bool)
constexpr uint32_t SET_BLIP_HUDMAP_ONLY   = 0x1E98AFEC;  // (Blip, bool)
constexpr uint32_t SET_BLIP_PAUSEMAP_ONLY = 0xFF3DB575;  // (Blip, bool)
constexpr uint32_t SET_BLIP_MAX_DISTANCE  = 0xCE79F8E2;  // (Blip, float)  < 0 = unlimited
constexpr uint32_t SET_BLIP_MIN_DISTANCE  = 0xFBA76D7E;  // (Blip, float)
constexpr uint32_t SET_BLIP_BLINK         = 0x04B8C8C6;  // (Blip, int, int, int)
constexpr uint32_t VDIST                  = 0x3C08ECB7;  // anchor for the self-check (handler 0x1401E6EF0)
}  // namespace nat

// joaat of the lowercased name: the hash the game registers natives under.
constexpr uint32_t NativeHash(const char* name) {
    uint32_t h = 0;
    for (; *name; ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h += static_cast<uint8_t>(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}
static_assert(NativeHash("ADD_BLIP_FOR_COORD") == nat::ADD_BLIP_FOR_COORD, "joaat");

// Handler for a native hash (nullptr = not registered / registry not built yet). Any thread; the registry is
// filled during the game's static init, long before the mod needs it, and never changes afterwards.
uintptr_t ResolveNative(uint32_t hash);

// Logs the registry location/size and checks it against the known VDIST handler. Returns false if the
// table does not look right (wrong game build): every Invoke() then fails cleanly.
bool CheckNativeRegistry();

// The call context the natives read (layout from the handlers, e.g. ADD_BLIP_FOR_COORD @0x140653B90):
//   +0x00 result*   +0x08 argc   +0x10 args* (8-byte slots)
//   +0x18 vector fix-up count, +0x20 pointers to the caller's vectors, +0x40 16-byte vector copies.
// Natives that take a Vector3* copy it into +0x40 and record it, so the context must be this big.
struct NativeContext {
    void* result = nullptr;
    uint32_t argc = 0;
    uint32_t pad0 = 0;
    uint64_t* args = nullptr;
    int32_t vecCount = 0;
    uint32_t pad1 = 0;
    float* vecRefs[4] = {};
    alignas(16) float vecData[4][4] = {};
    uint8_t spare[0x40] = {};  // headroom in case a native writes past the documented fields
};
static_assert(offsetof(NativeContext, args) == 0x10, "ctx layout");
static_assert(offsetof(NativeContext, vecCount) == 0x18, "ctx layout");
static_assert(offsetof(NativeContext, vecRefs) == 0x20, "ctx layout");
static_assert(offsetof(NativeContext, vecData) == 0x40, "ctx layout");

// One native call: push the arguments in order, then Call(hash). Floats/ints/bools/pointers take one 8-byte slot each.
class NativeCall {
  public:
    NativeCall& Int(int32_t v) { return Raw(static_cast<uint64_t>(static_cast<uint32_t>(v))); }
    NativeCall& Uint(uint32_t v) { return Raw(v); }
    NativeCall& Bool(bool v) { return Raw(v ? 1u : 0u); }
    NativeCall& Float(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        return Raw(bits);
    }
    NativeCall& Ptr(const void* p) { return Raw(reinterpret_cast<uint64_t>(p)); }
    NativeCall& Uint64(uint64_t v) { return Raw(v); }

    // Calls the native on the current thread (must be the game's main thread). False if the hash does not resolve.
    bool Call(uint32_t hash);

    uint64_t ResultRaw() const { return result_; }
    int32_t ResultInt() const { return static_cast<int32_t>(static_cast<uint32_t>(result_)); }
    uint32_t ResultUint() const { return static_cast<uint32_t>(result_); }
    bool ResultBool() const { return (result_ & 0xff) != 0; }
    float ResultFloat() const {
        float f = 0;
        uint32_t bits = static_cast<uint32_t>(result_);
        std::memcpy(&f, &bits, 4);
        return f;
    }

  private:
    NativeCall& Raw(uint64_t v) {
        if (argc_ < kMaxArgs) args_[argc_++] = v;
        return *this;
    }
    static constexpr int kMaxArgs = 32;
    uint64_t args_[kMaxArgs] = {};
    int argc_ = 0;
    uint64_t result_ = 0;
};

}  // namespace rdmp
