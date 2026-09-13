// --- src/sdk.h ---
// CS2 layout constants: entity-list structure + player skeleton bones.
#pragma once
#include <cstdint>

// Entity list layout. These are NOT offsets you dump -- they are the fixed
// structure of the list (this is exactly what the old calibrate() routine was
// discovering at runtime; now fixed so nothing ever "guesses" again).
namespace sdk {
    constexpr uintptr_t list_chunk_offset = 0x10;  // dwEntityList + 0x10 + 8*chunk
    constexpr uintptr_t list_entry_stride = 0x78;  // 120 bytes per slot
    constexpr uintptr_t bone_stride       = 0x20;  // 32 bytes per bone entry
}

// Bone indices into the bone array (stable across CS2 player models).
namespace bones {
    constexpr int pelvis      = 0;
    constexpr int spine_2     = 2;   // lower spine / stomach
    constexpr int spine_1     = 4;   // chest
    constexpr int neck_0      = 5;
    constexpr int head        = 6;

    constexpr int arm_upper_l = 8;
    constexpr int arm_lower_l = 9;
    constexpr int hand_l      = 10;

    constexpr int arm_upper_r = 13;
    constexpr int arm_lower_r = 14;
    constexpr int hand_r      = 15;

    constexpr int leg_upper_l = 22;
    constexpr int leg_lower_l = 23;
    constexpr int ankle_l     = 24;

    constexpr int leg_upper_r = 25;
    constexpr int leg_lower_r = 26;
    constexpr int ankle_r     = 27;

    // covers every id above (0..27)
    constexpr int count = 28;

    struct Link { int a, b; };
    constexpr Link kLinks[] = {
        { head,      neck_0     }, { neck_0,     spine_1    },
        { spine_1,   spine_2    }, { spine_2,    pelvis     },

        { spine_1,   arm_upper_l}, { arm_upper_l,arm_lower_l}, { arm_lower_l, hand_l },
        { spine_1,   arm_upper_r}, { arm_upper_r,arm_lower_r}, { arm_lower_r, hand_r },

        { pelvis,    leg_upper_l}, { leg_upper_l,leg_lower_l}, { leg_lower_l, ankle_l},
        { pelvis,    leg_upper_r}, { leg_upper_r,leg_lower_r}, { leg_lower_r, ankle_r},
    };
    constexpr int kNumLinks = sizeof(kLinks) / sizeof(kLinks[0]);
}
