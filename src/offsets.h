// --- src/offsets.h ---
// Client build: 2026-09-11.
#pragma once
#include <cstdint>

namespace offsets {
    // ── client.dll globals (verified working on your build) ───────────────
    constexpr uintptr_t dwEntityList      = 0x2577BE0;
    constexpr uintptr_t dwLocalPlayerPawn = 0x23CCC08;
    constexpr uintptr_t dwViewMatrix      = 0x23D21F0;

    // ── BHOP: cs2_dumper::buttons::jump ───────────────────────────────────
    // This value has to change after every CS2 update, which is why bhop keeps
    // dying. The value below is an ESTIMATE derived from your own build:
    //
    //   a2x dump 2026-07-29 : jump            = 0x2095490
    //   a2x dump 2026-08-25 : dwLocalPlayerPawn = 0x23C7268
    //   your build          : dwLocalPlayerPawn = 0x23CCC08
    //   -> your client.dll data is 0x59B0 higher, so:
    //      0x2095490 + 0x59B0 = 0x209AE40
    //
    // DO NOT trust it blindly. Verify it in-game:
    //   Menu -> [ Bhop ] -> hold W -> click [Detect Jump]
    // The menu prints the live dword at the address, so you can confirm by
    // releasing W and holding SPACE: the number must flip 0/256 <-> 65537.
    //
    // Manual value (Ctrl+F "jump ="):
    //   https://github.com/a2x/cs2-dumper/blob/main/output/buttons.hpp
    constexpr uintptr_t dwForceJump = 0x209AE40;

    // How far either side of dwForceJump the scanner looks.
    constexpr uintptr_t kJumpScanRadius = 0x10000;

    // ── C_BaseEntity / C_CSPlayerPawn ─────────────────────────────────────
    constexpr uintptr_t m_iHealth       = 0x34C;   // int32
    constexpr uintptr_t m_iTeamNum      = 0x3E7;   // uint8
    constexpr uintptr_t m_vOldOrigin    = 0x13B8;  // Vec3
    constexpr uintptr_t m_fFlags        = 0x3F4;   // FL_ONGROUND = (1 << 0)
    constexpr uintptr_t m_hGroundEntity = 0x50C;   // 0x8000 ground / 0xFFFFFFFF air

    // ── CCSPlayerController -> pawn handle ────────────────────────────────
    constexpr uintptr_t m_hPlayerPawn = 0x90C;

    // ── Skeleton ──────────────────────────────────────────────────────────
    // NOT hard-coded any more. m_pGameSceneNode and m_modelState move on
    // almost every update (m_modelState alone has shipped as 0x140, 0x150,
    // 0x160 and 0x190), which is exactly why your skeleton never drew.
    // esp.cpp now scans for the chain once at startup and validates it
    // geometrically against several real players before accepting it.
    //
    //   node = read(pawn + node_off)
    //   arr  = read(node + array_off)
    //   bone = read(arr  + i * m_boneStride)
    //
    // Widen these two ranges if a future update moves things further out.
    constexpr uintptr_t kSceneNodeScanBegin = 0x2C0;
    constexpr uintptr_t kSceneNodeScanEnd   = 0x380;
    constexpr uintptr_t kBoneArrayScanBegin = 0x180;
    constexpr uintptr_t kBoneArrayScanEnd   = 0x240;
    constexpr uintptr_t kScanStep           = 0x8;
    constexpr uintptr_t m_boneStride        = 0x20;
}
