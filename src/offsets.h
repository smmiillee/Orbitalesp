// --- src/offsets.h ---
// Globals: user-supplied offsets.hpp 2026-09-11 (client.dll)
// Schema:  confirmed from orbitalweb + a2x client_dll.hpp
#pragma once
#include <cstdint>

namespace offsets {
    // client.dll globals
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // C_BaseEntity / pawn
    constexpr uintptr_t m_iHealth         = 0x34C;
    constexpr uintptr_t m_iTeamNum        = 0x3E7;
    constexpr uintptr_t m_vOldOrigin      = 0x13B8;

    // CCSPlayerController -> pawn handle
    constexpr uintptr_t m_hPlayerPawn     = 0x90C;

    // Iteration
    constexpr int max_entities            = 64;
}
