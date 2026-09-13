// --- src/offsets.h ---
// Sourced from a2x/cs2-dumper — update after CS2 patches by re-running the dumper.
// Last synced: September 2026 build.
#pragma once
#include <cstdint>
#include <cstddef>

namespace offsets {

    // ── client.dll ────────────────────────────────────────────────────────────
    constexpr uintptr_t dwEntityList        = 0x2554050;
    constexpr uintptr_t dwLocalPlayerPawn   = 0x23A9118;
    constexpr uintptr_t dwLocalPlayerCtrl   = 0x2383DB0;
    constexpr uintptr_t dwViewMatrix        = 0x23AE550;

    // ── C_CSPlayerPawn ────────────────────────────────────────────────────────
    constexpr uintptr_t m_iHealth           = 0x344;
    constexpr uintptr_t m_iTeamNum          = 0x3E3;
    constexpr uintptr_t m_lifeState         = 0x348;
    constexpr uintptr_t m_pGameSceneNode    = 0x328;

    // ── CGameSceneNode ────────────────────────────────────────────────────────
    // m_nodeToWorld is the world-space origin of the node (Vec3 at offset 0xE0)
    constexpr uintptr_t m_nodeToWorld       = 0xE0;

    // ── controller -> pawn resolve ────────────────────────────────────────────
    // CCSPlayerController::m_hPlayerPawn (index handle, needs entity list resolve)
    constexpr uintptr_t m_hPlayerPawn       = 0x7E4;

    // ── entity iteration cap ──────────────────────────────────────────────────
    constexpr int max_entities              = 64;

} // namespace offsets
