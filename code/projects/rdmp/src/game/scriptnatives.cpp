// Native registry lookup + invoker (see scriptnatives.h and docs/SCRIPTING_RE.md "Native registry").
#include "scriptnatives.h"

#include <windows.h>

#include <atomic>

#include "log.h"
#include "memcheck.h"
#include "patch.h"

namespace rdmp {
namespace {

// Registry globals written by the registrar FUN_1401E7140 (build 0x673783F3).
constexpr uintptr_t kRegistryEntries = 0x142B0D210;  // u64: entry array
constexpr uintptr_t kRegistryCap = 0x142B0D218;      // u32: capacity (modulus)
constexpr uintptr_t kRegistryCount = 0x142B0D220;    // u32: registered natives
constexpr uintptr_t kVdistHandler = 0x1401E6EF0;     // VDIST, found while mapping the blip natives

struct Entry {
    uint32_t hash;
    uint32_t pad;
    uint64_t handler;
};
static_assert(sizeof(Entry) == 16, "registry entry");

bool InImageText(uintptr_t p) {
    const uintptr_t base = GameBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t lo = base + sec->VirtualAddress;
        if (p >= lo && p < lo + sec->Misc.VirtualSize) return true;
    }
    return false;
}

std::atomic<int> g_checked{0};  // 0 = not yet, 1 = ok, -1 = bad

// Natives whose registration never runs in the retail game (their registrar calls are skipped), so they are missing
// from the runtime table although their handlers are intact. Build 0x673783F3 addresses (re/natives_0x673783F3.csv);
// only used when the table lookup fails.
struct Unregistered {
    uint32_t hash;
    uintptr_t handler;
};
constexpr Unregistered kUnregistered[] = {
    {0xCE87DA6F, 0x140653F10},  // SET_BLIP_PRIORITY(blip, priority)
    // weapons, ammo and damage (game/weapons.cpp)
    {0x17883570, 0x1403BEAF0},  // GET_AMMOENUM_FOR_WEAPONENUM
    {0xD3E16075, 0x1402D92D0},  // GET_AMMO_ENUM
    {0x8266C617, 0x1403BE680},  // ACTOR_SET_WEAPON_AMMO
    {0x43DEDFAE, 0x1403BE9B0},  // ACTOR_GET_WEAPON_AMMO
    {0x4372593E, 0x1403BEEB0},  // ACTOR_SET_INV_AMMO
    {0xE224AC6F, 0x1403BF080},  // ACTOR_GET_INV_AMMO
    {0xCC69DCC1, 0x1403BE870},  // ACTOR_ADD_WEAPON_AMMO
    {0x98B3ABFA, 0x1403BEE00},  // ACTOR_ADD_INV_AMMO
    {0x4FE2B586, 0x1403BEFE0},  // ACTOR_SET_INV_AMMO_INFINITE
    {0xCB017277, 0x1403BDF30},  // DELETE_WEAPON_FROM_ACTOR
    {0xD695F857, 0x1403BF710},  // DELETE_ALL_WEAPONS_FROM_ACTOR
    {0x0D47CFBD, 0x1403BDF10},  // ACTOR_HAS_WEAPON
    {0x118D085E, 0x1403BF590},  // GET_NUM_WEAPONS_IN_INVENTORY
    {0xE38EF526, 0x140AD9320},  // SET_ACTOR_INVULNERABILITY
    {0xDB39D992, 0x140AD93E0},  // GET_ACTOR_INVULNERABILITY
    {0x2C0F211D, 0x140AD2C80},  // GET_LAST_ATTACKER
    {0x07B7AA6B, 0x140AD2CA0},  // GET_LAST_HIT_WEAPON
    {0x855F9A3B, 0x140AD2CD0},  // GET_LAST_HIT_ZONE
    {0x3A207AF2, 0x140AD2C90},  // GET_LAST_HIT_TIME
    {0x45556269, 0x140AD2CC0},  // GET_LAST_DAMAGE
    {0x8D696237, 0x140AD2EB0},  // CLEAR_LAST_HIT
};

uintptr_t ResolveUnregistered(uint32_t hash) {
    for (const auto& u : kUnregistered) {
        if (u.hash == hash) return InImageText(GameAddr(u.handler)) ? GameAddr(u.handler) : 0;
    }
    return 0;
}

}  // namespace

uintptr_t ResolveNative(uint32_t hash) {
    if (hash <= 1) return 0;
    const uintptr_t entriesSlot = GameAddr(kRegistryEntries);
    const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(entriesSlot);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(GameAddr(kRegistryCap));
    if (!entries || cap == 0 || cap > (1u << 20)) return 0;
    if (!IsReadable(entries, static_cast<size_t>(cap) * sizeof(Entry))) return 0;
    const auto* table = reinterpret_cast<const Entry*>(entries);
    uint32_t idx = hash % cap;
    const uint32_t step = (hash >> 1) + 1;
    for (uint32_t probes = 0; probes < cap; ++probes) {
        const uint32_t key = table[idx].hash;
        if (key == hash) {
            const uintptr_t fn = static_cast<uintptr_t>(table[idx].handler);
            return InImageText(fn) ? fn : 0;
        }
        if (key == 0) return ResolveUnregistered(hash);  // never-used slot ends the chain (1 = tombstone: keep probing)
        idx = static_cast<uint32_t>((static_cast<uint64_t>(idx) + step) % cap);
    }
    return ResolveUnregistered(hash);
}

bool CheckNativeRegistry() {
    const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(GameAddr(kRegistryEntries));
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(GameAddr(kRegistryCap));
    const uint32_t count = *reinterpret_cast<const uint32_t*>(GameAddr(kRegistryCount));
    const uintptr_t vdist = ResolveNative(nat::VDIST);
    const uintptr_t blip = ResolveNative(nat::ADD_BLIP_FOR_COORD);
    const bool ok = vdist == GameAddr(kVdistHandler) && blip != 0;
    Log("natives: registry %p capacity %u count %u; VDIST -> %p (want %p), ADD_BLIP_FOR_COORD -> %p: %s",
        reinterpret_cast<void*>(entries), cap, count, reinterpret_cast<void*>(vdist),
        reinterpret_cast<void*>(GameAddr(kVdistHandler)), reinterpret_cast<void*>(blip),
        ok ? "ok" : "MISMATCH (natives disabled: wrong game build?)");
    g_checked = ok ? 1 : -1;
    return ok;
}

bool NativeCall::Call(uint32_t hash) {
    if (g_checked.load() == 0) CheckNativeRegistry();
    if (g_checked.load() < 0) return false;
    const uintptr_t fn = ResolveNative(hash);
    if (!fn) {
        Log("natives: hash %08x is not registered", hash);
        return false;
    }
    NativeContext ctx;
    ctx.result = result_;
    ctx.argc = static_cast<uint32_t>(argc_);
    ctx.args = args_;
    std::memset(result_, 0, sizeof(result_));
    reinterpret_cast<void (*)(NativeContext*)>(fn)(&ctx);
    // Vector fix-up, as the VM does after a native returns: copy each recorded vector back to the caller's.
    for (int i = 0; i < ctx.vecCount && i < 4; ++i) {
        if (ctx.vecRefs[i]) std::memcpy(ctx.vecRefs[i], ctx.vecData[i], sizeof(float) * 3);
    }
    return true;
}

}  // namespace rdmp
