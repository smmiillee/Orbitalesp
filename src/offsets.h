#pragma once
#include <cstdint>

// ============================================================
// Offsets sourced from https://github.com/a2x/cs2-dumper
// UPDATE after every CS2 patch — run cs2-dumper.exe in-game.
// ============================================================

namespace offsets {
    constexpr uintptr_t dwEntityList            = 0x18C6CA8;
    constexpr uintptr_t dwLocalPlayerPawn       = 0x1880C60;
    constexpr uintptr_t dwLocalPlayerController = 0x1908A60;
    constexpr uintptr_t dwViewMatrix            = 0x19D2DC0;
    // dwForceJump removed — read-only build, no WPM
}

namespace cs2 {
    constexpr uintptr_t m_iTeamNum        = 0x3CB;
    constexpr uintptr_t m_iHealth         = 0x344;
    constexpr uintptr_t m_lifeState       = 0x348;
    constexpr uintptr_t m_vOldOrigin      = 0x127C;
    constexpr uintptr_t m_pGameSceneNode  = 0x328;
    constexpr uintptr_t m_hPlayerPawn     = 0x8FC;
    constexpr uintptr_t m_vecViewOffset   = 0xC58;
    constexpr uintptr_t m_modelState      = 0x170;
    constexpr uintptr_t m_boneArray       = 0x80;
    constexpr uintptr_t m_iszPlayerName   = 0x850;
    constexpr uintptr_t m_pClippingWeapon = 0x12F8;
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1B7A;
    constexpr uintptr_t m_flC4Blow        = 0x510;
}

namespace bones {
    constexpr int HEAD      = 6;
    constexpr int NECK      = 5;
    constexpr int SPINE1    = 4;
    constexpr int SPINE2    = 2;
    constexpr int PELVIS    = 0;
    constexpr int ARM_UP_L  = 8;
    constexpr int ARM_LO_L  = 9;
    constexpr int HAND_L    = 10;
    constexpr int ARM_UP_R  = 13;
    constexpr int ARM_LO_R  = 14;
    constexpr int HAND_R    = 15;
    constexpr int LEG_UP_L  = 22;
    constexpr int LEG_LO_L  = 23;
    constexpr int ANKLE_L   = 24;
    constexpr int LEG_UP_R  = 25;
    constexpr int LEG_LO_R  = 26;
    constexpr int ANKLE_R   = 27;
}

inline const char* WeaponName(uint16_t idx) {
    switch (idx) {
        case 1:  return "Desert Eagle";
        case 2:  return "Dual Berettas";
        case 3:  return "Five-SeveN";
        case 4:  return "Glock-18";
        case 7:  return "AK-47";
        case 8:  return "AUG";
        case 9:  return "AWP";
        case 10: return "FAMAS";
        case 11: return "G3SG1";
        case 13: return "Galil AR";
        case 14: return "M249";
        case 16: return "M4A4";
        case 17: return "MAC-10";
        case 19: return "P90";
        case 23: return "MP5-SD";
        case 24: return "UMP-45";
        case 25: return "XM1014";
        case 26: return "PP-Bizon";
        case 27: return "MAG-7";
        case 28: return "Negev";
        case 29: return "Sawed-Off";
        case 30: return "Tec-9";
        case 31: return "Zeus x27";
        case 32: return "P2000";
        case 33: return "MP7";
        case 34: return "MP9";
        case 35: return "Nova";
        case 36: return "P250";
        case 38: return "SCAR-20";
        case 39: return "SG 553";
        case 40: return "SSG 08";
        case 41: return "Knife";
        case 42: return "Flashbang";
        case 43: return "HE Grenade";
        case 44: return "Smoke";
        case 45: return "Molotov";
        case 46: return "Decoy";
        case 47: return "Incendiary";
        case 48: return "C4";
        case 60: return "M4A1-S";
        case 61: return "USP-S";
        case 63: return "CZ75-Auto";
        case 64: return "R8 Revolver";
        default: return "Unknown";
    }
}
