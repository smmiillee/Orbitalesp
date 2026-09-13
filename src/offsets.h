// --- src/offsets.h ---
// Client build: 2026-09.
//
// VERIFIED values come from the working Orbital Radar project
// (smmiillee/orbitalweb, src/memory_reader.cpp), which reads this same
// client.dll build. These are confirmed, not inferred.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals (VERIFIED) ─────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;
    constexpr uintptr_t dwPlantedC4       = 0x23973B8;   // VERIFIED

    // ── entity list ───────────────────────────────────────────────────────
    //   chunk = read(entity_list + kChunkOff + 8 * (idx >> 9))
    //   ent   = read(chunk + kSlotStride * (idx & 0x1FF))
    constexpr uintptr_t kChunkOff   = 0x10;
    constexpr uintptr_t kSlotStride = 120;  // 0x78 (112 = 0x70 on some builds)
    constexpr int       kChunks     = 4;
    constexpr int       kSlots      = 512;

    // ── C_BaseEntity (VERIFIED) ───────────────────────────────────────────
    constexpr uintptr_t m_iHealth        = 0x34C;
    constexpr uintptr_t m_iTeamNum       = 0x3E7;   // uint8
    constexpr uintptr_t m_vOldOrigin     = 0x13B8;  // Vec3
    constexpr uintptr_t m_pGameSceneNode = 0x338;   // VERIFIED (0x330 was wrong)
    constexpr uintptr_t m_angEyeAngles   = 0x3350;

    // ── C_BasePlayerPawn (VERIFIED) ───────────────────────────────────────
    // m_hController is a CHandle to CCSPlayerController -> this is the chain
    // that makes names work:  pawn -> controller -> m_iszPlayerName.
    constexpr uintptr_t m_hController     = 0x13D0;
    constexpr uintptr_t m_pWeaponServices = 0x1208;

    // ── CCSPlayerController (VERIFIED) ────────────────────────────────────
    constexpr uintptr_t m_iszPlayerName = 0x6F4;   // char[128]

    // ── econ item / weapon (VERIFIED) ─────────────────────────────────────
    // m_AttributeManager and m_Item are EMBEDDED structs, so the chain is:
    //   weapon + m_AttributeManager + m_Item + m_iItemDefinitionIndex
    constexpr uintptr_t m_hActiveWeapon        = 0x60;
    constexpr uintptr_t m_AttributeManager     = 0x1378;
    constexpr uintptr_t m_Item                 = 0x50;
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA;
    constexpr uint16_t  kItemDefC4             = 49;

    // ── CGameSceneNode (VERIFIED) ─────────────────────────────────────────
    // m_vecAbsOrigin: X at +0xC4, Y at +0xC8, Z at +0xCC.
    constexpr uintptr_t m_vecAbsOrigin = 0xC4;

    // ── BHOP: cs2_dumper::buttons::jump ───────────────────────────────────
    // NOT verified for this build, and it CANNOT be derived: between the
    // ExitScam dump (2026-07-09) and the a2x dump today, the button block
    // moved 0x1000 while the globals region moved ~0x22000. They are separate
    // regions, so the global delta is meaningless for buttons.
    //
    // Known values, newest first:
    //   0x2095490  a2x current       0x2094490  ExitScam 2026-07-09
    //   0x2093490  a2x              0x205BAF0  a2x early 2026
    //
    // bhop.cpp tries a candidate list and then sweeps kJumpScanLo..Hi, using
    // the user's real key presses as proof, so this value only needs to be
    // roughly right for the fast path.
    constexpr uintptr_t dwForceJump = 0x2095490;
    constexpr uintptr_t kJumpScanLo = 0x2060000;
    constexpr uintptr_t kJumpScanHi = 0x20E0000;

    // ── skeleton ──────────────────────────────────────────────────────────
    // The pointer chain is found at runtime; only the stride is fixed.
    constexpr uintptr_t m_boneStride   = 0x20;
    constexpr uintptr_t kPawnScanBytes = 0x900;
    constexpr uintptr_t kNodeScanBytes = 0x800;
    constexpr int       kBoneSlots     = 32;
}
