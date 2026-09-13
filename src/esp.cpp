#include "esp.h"
#include <imgui.h>

bool ESP::world_to_screen(const Vec3& w, Vec2& s, const Matrix4x4& vm, int sw, int sh) {
    float ww = vm.m[3][0] * w.x + vm.m[3][1] * w.y + vm.m[3][2] * w.z + vm.m[3][3];
    if (ww < 0.001f) return false;
    float x = vm.m[0][0] * w.x + vm.m[0][1] * w.y + vm.m[0][2] * w.z + vm.m[0][3];
    float y = vm.m[1][0] * w.x + vm.m[1][1] * w.y + vm.m[1][2] * w.z + vm.m[1][3];
    float inv_w = 1.0f / ww;
    s.x = (sw / 2.0f) + (x * inv_w) * (sw / 2.0f);
    s.y = (sh / 2.0f) - (y * inv_w) * (sh / 2.0f);
    return true;
}

static void calibrate(const Memory& mem, uintptr_t el, uintptr_t lpw, uintptr_t& best_off, uintptr_t& best_stride) {
    struct Combo { uintptr_t off; uintptr_t stride; };
    constexpr Combo combos[] = {{0x10, 112}, {0x10, 120}, {0x08, 112}, {0x08, 120}};
    int best_score = -1;
    best_off = 0x10; best_stride = 120;
    for (const auto& c : combos) {
        int score = 0;
        for (int chunk = 0; chunk < 4; chunk++) {
            uintptr_t cp = mem.read<uintptr_t>(el + c.off + 8 * chunk);
            if (!cp || cp < 0x10000) continue;
            for (int i = 0; i < 512; i++) {
                uintptr_t ent = mem.read<uintptr_t>(cp + c.stride * i);
                if (!ent || ent < 0x10000 || ent == lpw) continue;
                int hp = mem.read<int>(ent + offsets::m_iHealth);
                if (hp <= 0 || hp >
