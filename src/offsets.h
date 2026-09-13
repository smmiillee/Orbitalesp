// --- src/offsets.h ---
// Client build: 2026-09-11 offsets.
// Verified working: entity list, local pawn, view matrix, health, team, origin.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals ────────────────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // ── BHOP: jump button (cs2_dumper::buttons::jump) ─────────────────────
    // *** THIS WAS THE BHOP BUG ***
    // The old value 0xB3E00 is not a valid RVA for the jump button -- writes
    // landed in unrelated memory, so the game never saw a jump.
    // buttons::jump lives in the 0x209xxxx range on current builds:
    //   current a2x dump (2026-07-29): 0x2095490
    //   older builds:                  0x17348E0 / 0x1736920 / 0x186CD60
    //
    // If bhop stops working after a CS2 update, re-grab this ONE value:
    //   https://github.com/a2x/cs2-dumper/blob/main/output/buttons.hpp
    // (Ctrl+F for "jump =") and paste it here. Nothing else needs to change.
    constexpr uintptr_t dwForceJump = 0x2095490;

    // ── C_BaseEntity / C_CSPlayerPawn ─────────────────────────────────────
    constexpr uintptr_t m_iHealth       = 0x34C;   // int32
    constexpr uintptr_t m_iTeamNum      = 0x3E7;   // uint8
    constexpr uintptr_t m_vOldOrigin    = 0x13B8;  // Vec3
    constexpr uintptr_t m_fFlags        = 0x3F4;   // uint32, FL_ONGROUND = (1<<0)
    constexpr uintptr_t m_hGroundEntity = 0x50C;   // 0x8000 ground / 0xFFFFFFFF air

    // ── CCSPlayerController -> pawn handle ────────────────────────────────
    constexpr uintptr_t m_hPlayerPawn = 0x90C;

    // ── Skeleton: C_BaseEntity -> CSkeletonInstance -> CModelState ────────
    //   scene node = read(pawn + m_pGameSceneNode)
    //   bone array = read(node + m_modelState + m_boneArray)
    //   bone[i]    = read(bone array + i * m_boneStride)   // Vec3 at +0x000
    //
    // These are correct for the current build (they match the a2x dumps).
    // If the skeleton ever renders in the wrong place, exactly one of the
    // first two is off -- change it here and rebuild. Everything self-guards:
    // the head dot falls back to origin + 70 and the skeleton simply hides.
    constexpr uintptr_t m_pGameSceneNode = 0x330;
    constexpr uintptr_t m_modelState     = 0x190;
    constexpr uintptr_t m_boneArray      = 0x80;   // CSkeletonInstance::m_skeletonInstance
    constexpr uintptr_t m_boneStride     = 0x20;

    // ── iteration ─────────────────────────────────────────────────────────
    constexpr int max_entities = 64;
}
