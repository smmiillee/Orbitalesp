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

// entity_list + 8*((index>>9)+1) -> chunk
// chunk       + 0x78*(index&0x1FF) -> controller ptr
uintptr_t ESP::get_entity(const Memory& mem, uintptr_t entity_list, int index) const {
    uintptr_t chunk = mem.read<uintptr_t>(entity_list + 8 * ((index >> 9) + 1));
    if (!chunk) return 0;
    return mem.read<uintptr_t>(chunk + 0x78 * (index & 0x1FF));
}

// CHandle<> uint32: bits 0-14 = entity index — resolve through entity list
uintptr_t ESP::resolve_handle(const Memory& mem, uintptr_t entity_list, uint32_t handle) const {
    if (!handle || handle == 0xFFFFFFFF) return 0;
    int index = handle & 0x7FFF;
    uintptr_t chunk = mem.read<uintptr_t>(entity_list + 8 * ((index >> 9) + 1));
    if (!chunk) return 0;
    return mem.read<uintptr_t>(chunk + 0x78 * (index & 0x1FF));
}

void ESP::update(const Memory& mem, uintptr_t client_base) {
    players.clear();

    Matrix4x4 vm          = mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);
    uintptr_t entity_list = mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    uintptr_t local_pawn  = mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    if (!entity_list || !local_pawn) return;

    // Read screen dimensions — hardcoded here, main.cpp culls by actual res
    constexpr int W = 1920, H = 1080;

    for (int i = 0; i < offsets::max_entities; ++i) {
        uintptr_t controller = get_entity(mem, entity_list, i);
        if (!controller) continue;

        // Decode m_hPlayerPawn CHandle -> pawn pointer
        uint32_t pawn_handle = mem.read<uint32_t>(controller + offsets::m_hPlayerPawn);
        uintptr_t pawn = resolve_handle(mem, entity_list, pawn_handle);
        if (!pawn || pawn == local_pawn) continue;

        // Life check: 0 = alive
        uint8_t life = mem.read<uint8_t>(pawn + offsets::m_lifeState);
        if (life != 0) continue;

        int health = mem.read<int>(pawn + offsets::m_iHealth);
        if (health <= 0 || health > 100) continue;

        int team = mem.read<int>(pawn + offsets::m_iTeamNum);

        // m_vOldOrigin: world position directly on pawn — no scene node needed
        Vec3 origin = mem.read<Vec3>(pawn + offsets::m_vOldOrigin);
        if (origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f) continue;

        // Head: +70 units above feet
        Vec3 head = { origin.x, origin.y, origin.z + 70.0f };

        Vec2 screen_head, screen_feet;
        if (!world_to_screen(head,   screen_head, vm, W, H)) continue;
        if (!world_to_screen(origin, screen_feet, vm, W, H)) continue;

        float box_h = screen_feet.y - screen_head.y;
        if (box_h < 5.0f) continue;

        // Distance: Hammer units / 40 ≈ metres
        float dist = std::sqrt(
            origin.x * origin.x +
            origin.y * origin.y +
            origin.z * origin.z) / 40.0f;

        players.push_back({
            origin,
            screen_head,
            screen_feet,
            health,
            team,
            true,
            dist,
            box_h,
            box_h * 0.45f
        });
    }
}
