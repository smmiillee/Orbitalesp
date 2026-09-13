// --- src/offsets.h ---
// Client build: 2026-09.
//
// VERIFIED against the working Orbital Radar (smmiillee/orbitalweb,
// src/memory_reader.cpp) -- that file is the source of truth, since its own
// models/*.h only carries the four globals.
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
    // against the local player's own weapon. The designer-name string is only
    // the fallback -- I originally had this the wrong way round.
    constexpr uintptr_t m_hActiveWeapon = 0x60;

    // C_EconItem layout: AttributeManager and Item are EMBEDDED structs, so the
    // chain is a plain add: weapon + 0x1378 + 0x50 + 0x1BA.
    constexpr uintptr_t m_AttributeManager     = 0x1378;
    constexpr uintptr_t m_Item                 = 0x50;
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA;
    constexpr uint16_t  kItemDefC4             = 49;

    // Designer-name fallback chain: entity + 0x10 -> firstLevel,
    // firstLevel + 0x20 -> char* ("weapon_ak47").
    constexpr uintptr_t m_designerLvl1 = 0x10;
    constexpr uintptr_t m_designerPtr  = 0x20;

    // ── ground signals for bhop ───────────────────────────────────────────
    // Not verified for this build, so both are GRADED against the local
    // player's Z (m_vOldOrigin) before being trusted.
    constexpr uintptr_t m_fFlags        = 0x3F4;
    constexpr uintptr_t m_hGroundEntity = 0x530;

    // ── BHOP jump button ──────────────────────────────────────────────────
    // Not verifiable from a dump: the button block and the globals live in
    // different regions, so a global delta means nothing for buttons. bhop.cpp
    // finds it at runtime and requires a held key to confirm it.
    constexpr uintptr_t dwForceJump = 0x2095490;
    constexpr uintptr_t kJumpScanLo = 0x2060000;
    constexpr uintptr_t kJumpScanHi = 0x20E0000;

    // ── skeleton ──────────────────────────────────────────────────────────
    constexpr uintptr_t m_boneStride   = 0x20;
    constexpr uintptr_t kPawnScanBytes = 0x900;
    constexpr uintptr_t kNodeScanBytes = 0x800;
    constexpr int       kBoneSlots     = 32;
}
