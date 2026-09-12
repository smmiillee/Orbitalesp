#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

// CS2 subtick bhop via dwForceJump.
// Values: 0x10001 = +jump command, 0x1000100 = -jump command.
// We alternate on a 10ms cycle tied to SPACE — matches subtick
// input sampling rate, gives perfect hops without sv_autobunnyhopping.
void BhopTick() {
    static bool jumped = false;
    uintptr_t forceJumpAddr = g_Mem.clientBase + offsets::dwForceJump;

    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
        if (!jumped) {
            g_Mem.Write<int>(forceJumpAddr, 0x10001);   // +jump
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            g_Mem.Write<int>(forceJumpAddr, 0x1000100); // -jump
            jumped = true;
        }
    } else {
        jumped = false;
        // Release jump when space not held — prevents sticky jump state
        g_Mem.Write<int>(forceJumpAddr, 0x1000100);
    }
}
