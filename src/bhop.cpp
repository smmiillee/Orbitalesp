// --- src/bhop.cpp ---
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

constexpr uintptr_t m_hGroundEntity = 0x50C;

void BhopTick() {
    static bool wasInAir = false;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        wasInAir = false;
        return;
    }

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    uint32_t ground = g_mem.read<uint32_t>(local_pawn + m_hGroundEntity);
    bool inAir = (ground == 0xFFFFFFFF);

    if (wasInAir && !inAir) {
        // Landed — fire scroll down (requires mwheeldown bound to +jump in CS2)
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(-WHEEL_DELTA), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(-WHEEL_DELTA), 0);
    }

    wasInAir = inAir;
}
