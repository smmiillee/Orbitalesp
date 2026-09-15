// --- src/offsets.h ---
// Client build: 2026-09.
//
// VERIFIED values come from the working Orbital Radar (smmiillee/orbitalweb,
// src/memory_reader.cpp), which reads this same client.dll build. That file is
// the source of truth -- its own models/*.h carries only the four globals.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals (VERIFIED) ─────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;
    constexpr uintptr_t dwPlantedC4       = 0x23973B8;   // VERIFIED
    // NOTE: dwWeaponC4 (0x20E254C) is repurposed/stale in current builds --
    // the radar keeps it as a diagnostic only. Do not use it.

    // ── entity list ───────────────────────────────────────────────────────
    //   chunk = read(list + kChunkOff + 8 * (idx >> 9))
    //   ent   = read(chunk + kSlotStride * (idx & 0x1FF))
    constexpr uintptr_t kChunkOff   = 0x10;
    constexpr uintptr_t kSlotStride = 120;   // 0x78 (112 = 0x70 on some builds)
    // 8 chunks, not 4: the C4 entity can live past the first four.
    constexpr int       kChunks     = 8;
    constexpr int       kSlots      = 512;

    // ── C_BaseEntity / C_CSPlayerPawn (VERIFIED) ──────────────────────────
    constexpr uintptr_t m_iHealth        = 0x34C;   // int32
    constexpr uintptr_t m_iTeamNum       = 0x3E7;   // uint8
    constexpr uintptr_t m_vOldOrigin     = 0x13B8;  // Vec3
    constexpr uintptr_t m_pGameSceneNode = 0x338;

    // CGameSceneNode world position: X at +0xC4, then Y and Z contiguous.
    constexpr uintptr_t m_vecAbsOrigin = 0xC4;

    // ── C_BasePlayerPawn (VERIFIED) ───────────────────────────────────────
    // m_hController is a CHandle<CBasePlayerController>: pawn -> controller.
    // That direction is what makes NAMES work.
    constexpr uintptr_t m_hController     = 0x13D0;
    constexpr uintptr_t m_pWeaponServices = 0x1208;

    // ── CCSPlayerController (VERIFIED) ────────────────────────────────────
    constexpr uintptr_t m_iszPlayerName = 0x6F4;   // char[128]

    // ── active weapon + item definition (VERIFIED) ────────────────────────
    // The radar's PRIMARY weapon path is the definition index, self-tested
    // against the local player's own weapon; the designer-name string is only
    // the fallback.
    constexpr uintptr_t m_hActiveWeapon = 0x60;

    // Item definition index.
    //   m_iItemDefinitionIndex is a DIRECT field on the weapon entity at 0x1BA.
    //   Reading it only through the econ-item path returns a constant 0 for a
    //   weapon entity, which rendered every weapon as "#0".
    //
    // The econ path (AttributeManager + Item, both EMBEDDED) is kept as a
    // fallback only.
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA;
    constexpr uintptr_t m_AttributeManager     = 0x1378;
    constexpr uintptr_t m_Item                 = 0x50;
    constexpr uint16_t  kItemDefC4             = 49;

    // Designer-name fallback: entity + 0x10 -> firstLevel,
    // firstLevel + 0x20 -> char* ("weapon_ak47").
    constexpr uintptr_t m_designerLvl1 = 0x10;
    constexpr uintptr_t m_designerPtr  = 0x20;

    // ── ground signals ────────────────────────────────────────────────────
    // Not verified for this build, so both are GRADED against Z before being
    // trusted. Bit 0 of m_fFlags does NOT report FL_ONGROUND on the local
    // predicted pawn (it read 0x10000 while standing), which is why nothing
    // here may trust it directly.
    constexpr uintptr_t m_fFlags        = 0x3F4;
    constexpr uintptr_t m_hGroundEntity = 0x530;
    constexpr uint32_t  kFlagOnGround   = 1u << 0;
    constexpr uint32_t  kFlagDucking    = 1u << 2;
    constexpr uint32_t  kGroundEntityNone = 0xFFFFFFFFu;

    // ── model hitboxes ────────────────────────────────────────────────────
    // VERIFIED: CHitBox field layout, from Valve's schema dump
    // (SteamTracking/GameTracking-CS2, modellib/CHitBox.h).
    constexpr uintptr_t kHitBoxStride      = 0x50;
    constexpr uintptr_t kHitBoxMinBounds   = 0x18;  // Vector
    constexpr uintptr_t kHitBoxMaxBounds   = 0x24;  // Vector
    constexpr uintptr_t kHitBoxShapeRadius = 0x30;  // float32
    constexpr uintptr_t kHitBoxBoneHash    = 0x34;  // uint32
    constexpr uintptr_t kHitBoxGroupId     = 0x38;  // int32
    constexpr uintptr_t kHitBoxShapeType   = 0x3C;  // uint8
    constexpr uintptr_t kHitBoxIndex       = 0x48;  // uint16

    // CModelState::m_hModel -- a CStrongHandle, NOT a plain pointer, so it must
    // be resolved through the resource system. The deref chain is unverified;
    // hitbox.cpp tries several depths and validates the result.
    constexpr uintptr_t m_hModel = 0xA0;

    // ── CGameSceneNode -> CModelState ─────────────────────────────────────
    // NOT a single value. m_modelState has shipped as 0x140, 0x150, 0x160 and
    // 0x190 across builds, and on THIS build the bone array resolved at
    // node+0x1C0 -- which is inconsistent with 0x190 + 0x80 = 0x210. So the
    // hitbox code tries every candidate and validates, rather than assuming one.
    constexpr uintptr_t kModelStateCand[] = { 0x140, 0x150, 0x160, 0x170, 0x190 };
    constexpr int       kModelStateCandCount =
        static_cast<int>(sizeof(kModelStateCand) / sizeof(kModelStateCand[0]));

    // ── visibility (APPROXIMATE, see aim.cpp) ─────────────────────────────
    // m_entitySpottedState is RADAR state, not line of sight. Schema-confirmed
    // offset for a current build:
    //   C_CSPlayerPawn::m_entitySpottedState   = 0x1C60
    //   EntitySpottedState_t::m_bSpottedByMask = 0x0C  (uint32[2])
    // So the mask sits at pawn + 0x1C6C.
    //
    // Older builds used 0x23D0. If the vis check reports unavailable every
    // session, this offset has moved again.
    constexpr uintptr_t m_entitySpottedState = 0x1C60;
    constexpr uintptr_t m_bSpottedByMask     = 0x0C;

    // ── skeleton ──────────────────────────────────────────────────────────
    constexpr uintptr_t m_boneStride   = 0x20;
    constexpr uintptr_t kPawnScanBytes = 0x900;
    constexpr uintptr_t kNodeScanBytes = 0x800;
    constexpr int       kBoneSlots     = 32;
}
