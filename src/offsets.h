// --- src/offsets.h ---
// Globals:  user-supplied offsets.hpp 2026-09-11
// Schema:   a2x client_dll.hpp + confirmed working project values
#pragma once
#include <cstdint>

namespace offsets {

    // ── client.dll globals ────────────────────────────────────────────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwLocalPlayerCtrl = 0x23A78D0;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // ── C_BaseEntity / C_CSPlayerPawn (pawn offsets) ──────────────────────────
    constexpr uintptr_t m_iHealth         = 0x34C;  // int32
    constexpr uintptr_t m_lifeState       = 0x354;  // uint8,  0 = alive
    constexpr uintptr_t m_iTeamNum        = 0x3E7;  // uint8,  2=CT 3=T  (confirmed working)
    constexpr uintptr_t m_vOldOrigin      = 0x13B8; // Vector, world pos (confirmed working)

    // ── CCSPlayerController ───────────────────────────────────────────────────
    constexpr uintptr_t m_hPlayerPawn     = 0x90C;  // CHandle — decode via entity list

    // ── Iteration cap ─────────────────────────────────────────────────────────
    constexpr int max_entities            = 64;
}
