#pragma once
// The game's weapons (RDR.exe build 0x673783F3): WeaponModel ids as the natives take them, names as
// GET_WEAPON_ENUM_STRING_FROM_ENUM gives them (re/NOTES.md "Weapons"). Shared by the server and client script APIs.
#include <cctype>
#include <cstdint>
#include <string>

namespace rdmp::weapons {

constexpr int kNone = -1;  // nothing in hand
constexpr int kCount = 32;

inline constexpr const char* kNames[kCount] = {
    "PISTOL_Volcanic",       "PISTOL_SemiAuto",          "PISTOL_HighPower",     "PISTOL_Mauser",       "REVOLVER_Cattleman",
    "REVOLVER_Schofield",    "REVOLVER_DoubleAction",    "REVOLVER_LeMat",       "REPEATER_Carbine",    "REPEATER_Winchester",
    "REPEATER_Henry",        "REPEATER_Evans",           "RIFLE_Springfield",    "RIFLE_BoltAction",    "RIFLE_Buffalo",
    "SHOTGUN_SawedOff",      "SHOTGUN_DoubleBarrel",     "SHOTGUN_PumpAction",   "SHOTGUN_SemiAuto",    "SNIPERRIFLE_RollingBlock",
    "SNIPERRIFLE_Carcano",   "LASSO_lasso",              "MELEE_Knife",          "THROWN_FireBottle",   "THROWN_Dynamite",
    "THROWN_ThrowingKnife",  "TURRET_Gatling",           "TURRET_Browning",      "DEFAULT_Cannon",      "THROWN_Tomahawk",
    "BOW_Shortbow",          "RIFLE_Antitank",
};

inline bool Valid(int id) {
    return id >= 0 && id < kCount;
}

// A firearm (shoots rounds from a clip): pistols to sniper rifles, the turrets, the bow and the anti-tank rifle. Not the
// lasso, the knife or the thrown weapons.
inline bool IsGun(int id) {
    return (id >= 0 && id <= 20) || id == 26 || id == 27 || id == 28 || id == 30 || id == 31;
}

inline const char* Name(int id) {
    return Valid(id) ? kNames[id] : "";
}

// The id for a name: the game's ("REVOLVER_Cattleman") or just its model ("cattleman"), any case; -1 if unknown.
inline int FromName(const std::string& name) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string n = lower(name);
    for (int i = 0; i < kCount; ++i) {
        const std::string full = lower(kNames[i]);
        const size_t us = full.find('_');
        if (n == full || (us != std::string::npos && n == full.substr(us + 1))) return i;
    }
    return kNone;
}

}  // namespace rdmp::weapons
