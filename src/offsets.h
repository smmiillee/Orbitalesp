// --- src/offsets.h ---
// Globals: user-supplied offsets.hpp 2026-09-11 (client.dll)
// Schema: confirmed from orbitalweb + a2x client_dll.hpp
#pragma once
#include <cstdint>

namespace offsets {
    // client.dll globals  -- UNCHANGED
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // C_BaseEntity / pawn  -- UNCHANGED
    constexpr uintptr_t m_iHealth    = 0x34C;
    constexpr uintptr_t m_iTeamNum   = 0x3E7;
    constexpr uintptr_t m_vOldOrigin = 0x13B8;

    // CCSPlayerController -> pawn handle  -- UNCHANGED (0x90C)
    constexpr uintptr_t m_hPlayerPawn = 0x90C;

    // ─── NEW: added for skeleton ESP + head dots ───────────────────────────
    // Bone chain:
    //   boneArrayPtr = read<ptr>( pawn + m_pGameSceneNode )
    //   boneArray    = read<ptr>( sceneNode + m_modelState + m_boneArray )
    //   bonePos[i]   = read<Vec3>( boneArray + i * 0x20 )
    // Values per current a2x dumps. Verify against YOUR 2026-09-11 dump:
    //   grep for:  m_pGameSceneNode   (C_BaseEntity)
    //              m_modelState       (CSkeletonInstance)
    //   If the skeleton renders in the wrong place, ONE of these two is
    //   different in your build -- change it here and rebuild. Everything
    //   else self-guards and keeps working (dot falls back to origin+70).
    constexpr uintptr_t m_pGameSceneNode = 0x330;
    constexpr uintptr_t m_modelState     = 0x190;
    constexpr uintptr_t m_boneArray      = 0x80;
    // ────────────────────────────────────────────────────────────────────────

    // Iteration  -- UNCHANGED
    constexpr int max_entities = 64;
}
