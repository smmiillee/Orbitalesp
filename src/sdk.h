#pragma once
#include "memory.h"
#include "offsets.h"
#include "cs2math.h"
#include <string>
#include <vector>

struct BonePos {
    Vec3 pos;
};

struct Player {
    uintptr_t controller;   // CCSPlayerController
    uintptr_t pawn;         // C_CSPlayerPawn
    uintptr_t sceneNode;
    uintptr_t boneArray;

    int   team;
    int   health;
    uint8_t lifeState;      // 0 = alive

    Vec3  origin;           // feet
    Vec3  headPos;
    std::string name;
    std::string weapon;

    // Screen-space
    Vec2  screenFeet;
    Vec2  screenHead;
    bool  onScreen;

    // Bones (screen-space), indexed by bones::*
    Vec2  boneScreen[30];
    bool  boneOnScreen[30];

    bool IsAlive() const { return lifeState == 0 && health > 0; }
};

// Reads a bone world position from the bone array.
inline Vec3 GetBonePos(uintptr_t boneArray, int boneIdx) {
    // Each bone entry is 32 bytes: position at offset 0, then padding
    uintptr_t addr = boneArray + (boneIdx * 32);
    return g_Mem.Read<Vec3>(addr);
}

// Resolves entity handle → pawn pointer via entity list
inline uintptr_t HandleToPointer(uintptr_t entityList, uint32_t handle) {
    if (handle == 0xFFFFFFFF) return 0;
    uint32_t index = handle & 0x7FFF;
    uintptr_t listEntry = g_Mem.Read<uintptr_t>(entityList + 0x8 * ((index & 0x7FFF) >> 9) + 0x10);
    if (!listEntry) return 0;
    return g_Mem.Read<uintptr_t>(listEntry + 0x78 * (index & 0x1FF));
}

// Reads all player data from CS2 process memory.
inline std::vector<Player> GetPlayers(const Matrix4x4& vm, int W, int H) {
    std::vector<Player> players;

    uintptr_t entityList = g_Mem.clientBase + offsets::dwEntityList;
    uintptr_t localPawn  = g_Mem.Read<uintptr_t>(g_Mem.clientBase + offsets::dwLocalPlayerPawn);
    int localTeam = g_Mem.Read<int>(localPawn + cs2::m_iTeamNum);

    // Entity list has 64 controller slots at list[0]
    uintptr_t listBase = g_Mem.Read<uintptr_t>(entityList + 0x10);
    if (!listBase) return players;

    for (int i = 1; i < 64; i++) {
        uintptr_t ctrl = g_Mem.Read<uintptr_t>(listBase + 0x78 * i);
        if (!ctrl) continue;

        uint32_t pawnHandle = g_Mem.Read<uint32_t>(ctrl + cs2::m_hPlayerPawn);
        uintptr_t pawn = HandleToPointer(entityList, pawnHandle);
        if (!pawn || pawn == localPawn) continue;

        Player p{};
        p.controller = ctrl;
        p.pawn       = pawn;
        p.team       = g_Mem.Read<int>(pawn + cs2::m_iTeamNum);
        p.health     = g_Mem.Read<int>(pawn + cs2::m_iHealth);
        p.lifeState  = g_Mem.Read<uint8_t>(pawn + cs2::m_lifeState);

        if (!p.IsAlive()) continue;

        p.origin     = g_Mem.Read<Vec3>(pawn + cs2::m_vOldOrigin);
        p.name       = g_Mem.ReadString(ctrl + cs2::m_iszPlayerName, 64);

        // Bone array
        p.sceneNode  = g_Mem.Read<uintptr_t>(pawn + cs2::m_pGameSceneNode);
        uintptr_t modelState = p.sceneNode + cs2::m_modelState;
        p.boneArray  = g_Mem.Read<uintptr_t>(modelState + cs2::m_boneArray);

        // Head world position from bone array
        p.headPos = GetBonePos(p.boneArray, bones::HEAD);

        // Weapon
        uintptr_t weaponEnt = g_Mem.Read<uintptr_t>(pawn + cs2::m_pClippingWeapon);
        if (weaponEnt) {
            uint16_t defIdx = g_Mem.Read<uint16_t>(weaponEnt + cs2::m_iItemDefinitionIndex);
            p.weapon = WeaponName(defIdx);
        } else {
            p.weapon = "Knife";
        }

        // W2S
        p.onScreen = WorldToScreen(p.origin, p.screenFeet, vm, W, H) &&
                     WorldToScreen(p.headPos, p.screenHead, vm, W, H);

        // Bone screen positions
        const int BONE_LIST[] = {
            bones::HEAD, bones::NECK, bones::SPINE1, bones::SPINE2, bones::PELVIS,
            bones::ARM_UP_L, bones::ARM_LO_L, bones::HAND_L,
            bones::ARM_UP_R, bones::ARM_LO_R, bones::HAND_R,
            bones::LEG_UP_L, bones::LEG_LO_L, bones::ANKLE_L,
            bones::LEG_UP_R, bones::LEG_LO_R, bones::ANKLE_R
        };
        for (int b : BONE_LIST) {
            Vec3 boneW = GetBonePos(p.boneArray, b);
            p.boneOnScreen[b] = WorldToScreen(boneW, p.boneScreen[b], vm, W, H);
        }

        players.push_back(p);
    }

    return players;
}
