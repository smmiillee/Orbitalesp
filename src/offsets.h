// --- src/offsets.h ---
// Client build: 2026-09. Values marked DUMP come from the a2x cs2-dumper.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals (verified working on your build) ───────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // ── BHOP: cs2_dumper::buttons::jump ───────────────────────────────────
    // DUMP: jump = 0x2095490, on the 0x90 button stride. The whole block:
    //   sprint-0x630  reload-0x5A0  attack-0x510  attack2-0x480
    //   turnleft-0x3F0  turnright-0x360  forward-0x2D0  back-0x240
    //   left-0x1B0  right-0x120  use-0x090  JUMP  duck+0x090
    //
    // bhop.cpp does NOT trust this value: it re-finds the block at runtime,
    // because this RVA moves on essentially every CS2 update.
    constexpr uintptr_t dwForceJump = 0x2095490;

    // Window the runtime scanner sweeps, relative to client.dll. Wide enough
    // to cover the two most recent dumps (0x2093490 and 0x2095490) plus slack.
    constexpr uintptr_t kJumpScanLo = 0x2060000;
    constexpr uintptr_t kJumpScanHi = 0x20E0000;

    // ── entity list layout ────────────────────────────────────────────────
    //   chunk = read(entity_list + kChunkOff + 8 * (idx >> 9))
    //   ent   = read(chunk + kSlotStride * (idx & 0x1FF))
    constexpr uintptr_t kChunkOff   = 0x10;
    constexpr uintptr_t kSlotStride = 120;  // 0x78 (112 = 0x70 on some builds)
    constexpr int       kChunks     = 4;
    constexpr int       kSlots      = 512;

    // ── C_BaseEntity ──────────────────────────────────────────────────────
    constexpr uintptr_t m_iHealth       = 0x34C;
    constexpr uintptr_t m_iTeamNum      = 0x3E7;
    constexpr uintptr_t m_fFlags        = 0x3F4;
    constexpr uintptr_t m_hGroundEntity = 0x530;
    constexpr uintptr_t m_vOldOrigin    = 0x13B8;
    constexpr uintptr_t m_pGameSceneNode = 0x330;

    // ── skeleton ──────────────────────────────────────────────────────────
    // The pointer chain is found at runtime; only the stride is fixed.
    constexpr uintptr_t m_boneStride    = 0x20;
    constexpr uintptr_t kPawnScanBytes  = 0x900;
    constexpr uintptr_t kNodeScanBytes  = 0x800;
    constexpr int       kBoneSlots      = 32;

    // ── planted C4 ────────────────────────────────────────────────────────
    // Estimate only. The bomb code validates the scene node and position at
    // runtime, so a wrong value here hides the bomb instead of drawing it in
    // the wrong place.
    constexpr uintptr_t dwPlantedC4 = 0x2397200;
}
