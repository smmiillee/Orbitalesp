// --- src/offsets.h ---
// Global offsets from user-supplied offsets.hpp (a2x/cs2-dumper 2026-09-11)
// Schema offsets from a2x/cs2-dumper client_dll.hpp (confirmed live)
#pragma once
#include <cstdint>
#include <cstddef>

namespace offsets {

    // ── client.dll globals ────────────────────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwLocalPlayerCtrl = 0x23A78D0;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // ── C_BaseEntity ──────────────────────────────────────────────────────────
    constexpr uintptr_t m_pGameSceneNode  = 0x330; // CGameSceneNode*
    constexpr uintptr_t m_iHealth         = 0x34C; // int32
    constexpr uintptr_t m_lifeState       = 0x354; // uint8  (0 = alive)
    constexpr uintptr_t m_iTeamNum        = 0x3EB; // uint8  (2=CT, 3=T)

    // ── CCSPlayerController ───────────────────────────────────────────────────
    constexpr uintptr_t m_hPlayerPawn     = 0x90C; // handle -> pawn

    // ── CGameSceneNode ────────────────────────────────────────────────────────
    // m_nodeToWorld: the node-to-world transform matrix starts at 0x10.
    // The Vec3 origin sits at the first 12 bytes (x,y,z floats).
    constexpr uintptr_t m_nodeToWorld     = 0x10;  // Vec3 origin

    // ── Entity iteration cap ──────────────────────────────────────────────────
    constexpr int max_entities            = 64;

} // namespace offsets
