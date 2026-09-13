// --- src/offsets.h ---
// Client build: 2026-09.
//
// All values below are VERIFIED against the working Orbital Radar
// (smmiillee/orbitalweb, src/memory_reader.cpp), which reads this same build.
// The radar's own models/*.h only carries the four globals -- its comment says
// the rest were moved into the reader "for reliability", so that file is the
// source of truth.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals (VERIFIED) ─────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;
    constexpr uintptr_t dwPlantedC4       = 0x23973B8;   // VERIFIED

    // ── entity list ───────────────────────────────────────────────────────
    //   chunk = read(list + kChunkOff + 8 * (idx >> 9))
    //   ent   = read(chunk + kSlotStride * (idx & 0x1FF))
    constexpr uintptr_t kChunkOff   = 0x10;
    constexpr uintptr_t kSlotStride = 120;   // 0x78 (112 = 0x70 on some builds)
    constexpr int       kChunks     = 4;
    constexpr int       kSlots      = 512;

    // ── C_BaseEntity / C_CSPlayerPawn (VERIFIED) ──────────────────────────
    constexpr uintptr_t m_iHealth        = 0x34C;   // int32
    constexpr uintptr_t m_iTeamNum       = 0x3E7;   // uint8
    constexpr uintptr_t m_vOldOrigin     = 0x13B8;  // Vec3
    constexpr uintptr_t m_pGameSceneNode = 0x338;

    // CGameSceneNode world position: X at +0xC4, Y at +0xC8, Z at +0xCC.
    // This is the chain the radar uses for every world position.
    constexpr uintptr_t m_vecAbsOrigin = 0xC4;

    // ── C_BasePlayerPawn (VERIFIED) ───────────────────────────────────────
    // m_hController is a CHandle<CBasePlayerController> and points pawn ->
    // controller. That direction is what makes NAMES work:
    //   pawn + m_hController -> handle -> resolve -> controller + name
    constexpr uintptr_t m_hController     = 0x13D0;
    constexpr uintptr_t m_pWeaponServices = 0x1208;

    // ── CCSPlayerController (VERIFIED, schema-confirmed) ──────────────────
    constexpr uintptr_t m_iszPlayerName = 0x6F4;   // char[128]

    // ── active weapon (VERIFIED) ──────────────────────────────────────────
    constexpr uintptr_t m_hActiveWeapon = 0x60;

    // Designer-name chain -- this is how the radar reads weapon names, and it
    // is far more reliable than mapping item-definition indices to a name table:
    //   weapon + 0x10 -> firstLevel ; firstLevel + 0x20 -> char* "weapon_ak47"
    constexpr uintptr_t m_designerLvl1 = 0x10;
    constexpr uintptr_t m_designerPtr  = 0x20;

    // C_EconItem layout: AttributeManager and Item are EMBEDDED structs, so the
    // def-index chain is weapon + m_AttributeManager + m_Item + m_iItemDef.
    // Used only as a secondary C4 check.
    constexpr uintptr_t m_AttributeManager     = 0x1378;
    constexpr uintptr_t m_Item                 = 0x50;
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA;
    constexpr uint16_t  kItemDefC4             = 49;

    // ── ground signals for bhop ───────────────────────────────────────────
    // Neither is verified for this build, so both are GRADED against the local
    // player's Z (m_vOldOrigin) before being trusted -- a wrong value degrades
    // the verdict instead of breaking it.
    constexpr uintptr_t m_fFlags        = 0x3F4;
    constexpr uintptr_t m_hGroundEntity = 0x530;

    // ── BHOP jump button ──────────────────────────────────────────────────
    // NOT verified, and it CANNOT be derived: between the ExitScam dump
    // (2026-07-09) and the a2x dump the button block moved 0x1000 while the
    // globals region moved ~0x22000. Separate regions, so a global delta is
    // meaningless for buttons. bhop.cpp finds it at runtime and now requires a
    // confirmation from a key you are actually holding before locking.
    constexpr uintptr_t dwForceJump = 0x2095490;
    constexpr uintptr_t kJumpScanLo = 0x2060000;
    constexpr uintptr_t kJumpScanHi = 0x20E0000;

    // ── skeleton ──────────────────────────────────────────────────────────
    // Only the stride is fixed; the pointer chain is found at runtime.
    constexpr uintptr_t m_boneStride   = 0x20;
    constexpr uintptr_t kPawnScanBytes = 0x900;
    constexpr uintptr_t kNodeScanBytes = 0x800;
    constexpr int       kBoneSlots     = 32;
}
