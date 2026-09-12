#pragma once
#include <cmath>
#include <cstdint>

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    float Length() const { return sqrtf(x*x + y*y + z*z); }
};

struct Vec2 {
    float x, y;
    Vec2() : x(0), y(0) {}
    Vec2(float x, float y) : x(x), y(y) {}
};

struct Matrix4x4 {
    float m[4][4];
};

// Projects a 3D world position to 2D screen coordinates.
// Returns false if the point is behind the camera.
inline bool WorldToScreen(const Vec3& world, Vec2& screen,
                          const Matrix4x4& vm, int W, int H) {
    float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
            + vm.m[3][2] * world.z + vm.m[3][3];

    if (w < 0.001f) return false;

    float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
            + vm.m[0][2] * world.z + vm.m[0][3];
    float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
            + vm.m[1][2] * world.z + vm.m[1][3];

    screen.x = (W * 0.5f) + (x / w) * (W  * 0.5f);
    screen.y = (H * 0.5f) - (y / w) * (H  * 0.5f);
    return true;
}

inline float Distance3D(const Vec3& a, const Vec3& b) {
    return (a - b).Length();
}
