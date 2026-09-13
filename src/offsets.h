// --- src/offsets.h ---
// Client build: 2026-09. Offsets marked SCHEMA come from the Source 2 client
// schema dump (Aug 2026 revision) and are confirmed, not guessed.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals ────────────────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // Bhop jump button. Reads 256 (-jump) live, so this estimate is already
    // correct; the scanner just confirms it. Re-grab after a patch:
    //   https://github.com/a2x/cs2-dumper/blob/main/output/buttons.hpp
    constexpr uintptr_t dwForceJump     = 0x209AE40;
    constexpr uintptr_t kJumpScanRadius = 0x10000;
    constexpr uintptr_t kScanStep       = 0x8;

    // ── entity list layout ────────────────────────────────────────────────
    constexpr uintptr_t kChunkOff   = 0x10;
    constexpr uintptr_t kSlotStride = 120;  // 0x78
    constexpr int       kChunks     = 4;
    constexpr int       kSlots      = 512;

    // ── C_BaseEntity ──────────────────────────────────────────────────────
    constexpr uintptr_t m_iHealth    = 0x34C;   // SCHEMA
    constexpr uintptr_t m_iTeamNum   = 0x3E7;   // SCHEMA (uint8)
    constexpr uintptr_t m_fFlags     = 0x3F4;   // SCHEMA (uint32)
    constexpr uintptr_t m_hGroundEntity = 0x530; // inferred from field order
    constexpr uintptr_t m_vOldOrigin = 0x13B8;

    // ── C_BaseEntity -> CGameSceneNode (needed for the planted C4) ────────
    // Schema-confirmed. This is also the entry point of the skeleton chain,
    // but the skeleton re-derives its own chain at runtime, so the two are
    // independent -- a wrong value here only affects the bomb.
    constexpr uintptr_t m_pGameSceneNode = 0x330;

    // ── C_BasePlayerPawn ──────────────────────────────────────────────────
    constexpr uintptr_t m_pWeaponServices = 0x11E0;  // candidate, validated
    constexpr uintptr_t m_hActiveWeapon   = 0x60;

    // ── CCSPlayerController ───────────────────────────────────────────────
    constexpr uintptr_t m_iszPlayerName = 0x6F4;   // SCHEMA, char[128]

    // ── weapon attribute chain (candidates, consensus-validated at runtime) ─
    constexpr uintptr_t m_AttributeItem        = 0x50;
    constexpr uintptr_t m_iItemDefinitionIndex = 0x1BA;

    // ── planted C4 ────────────────────────────────────────────────────────
    // Estimate interpolated from the last dump via the delta of the globals we
    // DO have. Validated at runtime: a wrong value reads a non-entity or a
    // non-stable position, and the bomb simply doesn't draw.
    constexpr uintptr_t dwPlantedC4 = 0x2397200;

    // ── skeleton ──────────────────────────────────────────────────────────
    // The pointer chain is found at runtime, so only the search windows and
    // the bone stride live here.
    constexpr uintptr_t m_boneStride    = 0x20;
    constexpr uintptr_t kPawnScanBytes  = 0x900;
    constexpr uintptr_t kNodeScanBytes  = 0x800;
    constexpr int       kBoneSlots      = 32;
}
