// --- src/esp.cpp ---
#include "esp.h"
#include <imgui.h>

bool ESP::world_to_screen(const Vec3& world, Vec2& screen,
    const Matrix4x4& vm, int screen_w, int screen_h) {
    float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
        + vm.m[3][2] * world.z + vm.m[3][3];
    if (w < 0.001f) return false;

    float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
        + vm.m[0][2] * world.z + vm.m[0][3];
    float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
        + vm.m[1][2] * world.z + vm.m[1][3];

    float inv_w = 1.0f / w;
    screen.x = (screen_w / 2.0f) + (x * inv_w) * (screen_w / 2.0f);
    screen.y = (screen_h / 2.0f) - (y * inv_w) * (screen_h / 2.0f);
    return true;
}

// Calibration function - same as before
static void calibrate(const Memory& mem, uintptr_t entity_list,
    uintptr_t local_pawn,
    uintptr_t& best_off, uintptr_t& best_stride) {
    struct Combo { uintptr_t off; uintptr_t stride; };
    constexpr Combo combos[] = {
        {0x10, 112}, {0x10, 120}, {0x08, 
