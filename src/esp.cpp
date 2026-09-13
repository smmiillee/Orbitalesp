// --- src/esp.cpp ---
#include "esp.h"
#include <cmath>

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

void ESP::update(const Memory& mem, uintptr_t client_base) {
    players.clear();
    debug_total_controllers = 0;
    debug_valid_pawns       = 0;
    debug_alive             = 0;
    debug_positioned        = 0;
    debug_on_screen         = 0;
    debug_sample_health     = 0;
    debug_sample_team       = 0;

    Matrix4x4 vm          = mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);
    uintptr_t entity_list = mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    uintptr_t local_pawn  = mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    if (!entity_list || !local_pawn) return;

    constexpr int W = 1920, H = 1080;

    for (int chunk = 0; chunk < 4; chunk++) {
        uintptr_t chunk_ptr = mem.read<uintptr_t>(entity_list + 0x10 + 8 * chunk);
        if (!chunk_ptr || chunk_ptr < 0x10000) continue;

        for (int i = 0; i < 512; i++) {
            uintptr_t entity = mem.read<uintptr_t>(chunk_ptr + 120 * i);
            if (!entity || entity < 0x10000) continue;
            if (entity == local_pawn) continue;
            debug_total_controllers++;

            int team = mem.read<int>(entity + offsets::m_iTeamNum) & 0xFF;
            if (team != 2 && team != 3) continue;
            debug_valid_pawns++;

            // Sample first match for debug display
            if (debug_valid_pawns == 1) {
                debug_sample_health = mem.read<int>(entity + offsets::m_iHealth);
                debug_sample_team   = team;
            }

            // These are controllers — need to resolve to pawn via m_hPlayerPawn
            // Controller offset 0x90C holds CHandle to the pawn
            uint32_t pawn_handle = mem.read<uint32_t>(entity + offsets::m_hPlayerPawn);
            if (!pawn_handle || pawn_handle == 0xFFFFFFFF) continue;

            // Resolve handle: index = handle & 0x7FFF
            int pawn_index = pawn_handle & 0x7FFF;
            uintptr_t pawn_chunk = mem.read<uintptr_t>(
                entity_list + 0x10 + 8 * (pawn_index >> 9));
            if (!pawn_chunk || pawn_chunk < 0x10000) continue;

            uintptr_t pawn = mem.read<uintptr_t>(
                pawn_chunk + 120 * (pawn_index & 0x1FF));
            if (!pawn || pawn < 0x10000 || pawn == local_pawn) continue;

            int health = mem.read<int>(pawn + offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;
            debug_alive++;

            Vec3 origin;
            origin.x = mem.read<float>(pawn + offsets::m_vOldOrigin);
            origin.y = mem.read<float>(pawn + offsets::m_vOldOrigin + 4);
            origin.z = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
            if (origin.x == 0.0f && origin.y == 0.0f) continue;
            debug_positioned++;

            int pawn_team = mem.read<int>(pawn + offsets::m_iTeamNum) & 0xFF;

            Vec3 head = { origin.x, origin.y, origin.z + 70.0f };
            Vec2 screen_head, screen_feet;
            if (!world_to_screen(head,   screen_head, vm, W, H)) continue;
            if (!world_to_screen(origin, screen_feet, vm, W, H)) continue;

            float box_h = screen_feet.y - screen_head.y;
            if (box_h < 5.0f) continue;
            debug_on_screen++;

            float dist = std::sqrt(
                origin.x * origin.x +
                origin.y * origin.y +
                origin.z * origin.z) / 40.0f;

            players.push_back({
                origin, screen_head, screen_feet,
                health, pawn_team, true, dist,
                box_h, box_h * 0.45f
            });
        }
    }
}
