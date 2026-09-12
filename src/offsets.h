// --- src/offsets.h ---
#pragma once
#include <cstdint>

// CS2 offsets — update from https://github.com/a2x/cs2-dumper when Valve patches
namespace offsets {
    // client.dll
    constexpr uintptr_t dwEntityList     = 0x18C0E18;
    constexpr uintptr_t dwLocalPlayerPawn= 0x17399D0;
    constexpr uintptr_t dwViewMatrix     = 0x19237A0;

    // entity
    constexpr uintptr_t m_iHealth        = 0x344;
    constexpr uintptr_t m_iTeamNum       = 0x3CB;
    constexpr uintptr_t m_lifeState      = 0x348;
    constexpr uintptr_t m_vecOrigin      = 0x127;  // via CBodyComponentBaseAnimGraph
    constexpr uintptr_t m_pGameSceneNode = 0x310;
    constexpr uintptr_t m_nodeToWorld    = 0x80;   // offset within CGameSceneNode

    // entity list controller
    constexpr uintptr_t m_pChunks        = 0x0;
    constexpr ptrdiff_t chunk_size        = 0x10;
    constexpr int       max_entities      = 64;
}
