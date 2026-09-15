// --- src/hitbox.cpp ---
#include "hitbox.h"
#include "offsets.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace {

std::atomic<int>  g_attempts{0};
std::atomic<bool> g_ready{false};
const char* g_status = "not attempted";

bool valid_ptr(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFF0000ULL && (p % 4) == 0;
}

bool sane(float f, float lim) {
    return std::isfinite(f) && std::fabs(f) < lim;
}

// One CHitBox as read from memory.
struct RawHitbox {
    Vec3   min{};
    Vec3   max{};
    float  radius = 0.0f;
    uint32_t bone_hash = 0;
    int    group = 0;
    uint8_t shape = 0;
    uint16_t index = 0;
};

// CHitBox is stride 0x50 with the fields at the offsets above.
void read_hitbox(const Memory& mem, uintptr_t base, RawHitbox& out) {
    mem.read_bytes(base + 0x18, &out.min, sizeof(Vec3));
    mem.read_bytes(base + 0x24, &out.max, sizeof(Vec3));
    out.radius    = mem.read<float>(base + 0x30);
    out.bone_hash = mem.read<uint32_t>(base + 0x34);
    out.group     = mem.read<int>(base + 0x38);
    out.shape     = mem.read<uint8_t>(base + 0x3C);
    out.index     = mem.read<uint16_t>(base + 0x48);
}

// Does this look like a real CHitBox?
bool hitbox_plausible(const RawHitbox& h) {
    if (!sane(h.min.x, 200.0f) || !sane(h.min.y, 200.0f) ||
        !sane(h.min.z, 200.0f)) return false;
    if (!sane(h.max.x, 200.0f) || !sane(h.max.y, 200.0f) ||
        !sane(h.max.z, 200.0f)) return false;

    // A real hitbox has some extent.
    const float ext =
        std::fabs(h.max.x - h.min.x) + std::fabs(h.max.y - h.min.y) +
        std::fabs(h.max.z - h.min.z);
    if (ext < 0.5f || ext > 300.0f) return false;

    if (!sane(h.radius, 100.0f)) return false;
    if (h.radius < 0.0f) return false;

    // Shape types seen in Source 2 models.
    if (h.shape > 6) return false;

    return true;
}

// Validate a candidate hitbox-set array: count plus every entry plausible.
bool validate_set(const Memory& mem, uintptr_t arr, int count) {
    if (!valid_ptr(arr)) return false;
    if (count < 8 || count > 40) return false;

    int good = 0;
    for (int i = 0; i < count; ++i) {
        RawHitbox h{};
        read_hitbox(mem, arr + (uintptr_t)i * 0x50, h);
        if (!hitbox_plausible(h)) continue;
        ++good;
    }
    // Most of the set has to be sane, or this is not a hitbox array.
    return good >= count - 2;
}

} // namespace

void Hitbox_Init() {
    g_status = "not attempted";
}

const char* Hitbox_Status() { return g_status; }
int  Hitbox_Attempts()      { return g_attempts.load(); }

// ---------------------------------------------------------------------------
// The attempt. Everything here is unverified, so the ONLY thing that matters is
// whether a candidate survives validate_set(). Nothing is returned unless it
// does, and the caller falls back to the bone mesh otherwise.
// ---------------------------------------------------------------------------
bool Hitbox_Get(const Memory& mem, uintptr_t pawn, HitboxSetResult& out) {
    out.ready = false;
    out.count = 0;
    if (!valid_ptr(pawn)) return false;

    const uintptr_t node =
        mem.read<uintptr_t>(pawn + offsets::m_pGameSceneNode);
    if (!valid_ptr(node)) return false;

    const uintptr_t model_state = node + offsets::m_modelState;
    if (!valid_ptr(model_state)) return false;

    // ---- attempt 1: resolve m_hModel (CStrongHandle) and scan for the set ----
    // A CStrongHandle is not a plain pointer, so a few dereference depths are
    // tried. Whatever we land on still has to pass validate_set(), which is what
    // keeps a wrong deref from being used.
    const uintptr_t handle =
        mem.read<uintptr_t>(model_state + offsets::m_hModel);
    if (!valid_ptr(handle)) {
        g_status = "m_hModel handle not readable";
        g_attempts.fetch_add(1);
        return false;
    }

    uintptr_t models[4] = { handle, 0, 0, 0 };
    models[1] = mem.read<uintptr_t>(handle);
    if (valid_ptr(models[1]))
        models[2] = mem.read<uintptr_t>(models[1]);
    if (valid_ptr(models[2]))
        models[3] = mem.read<uintptr_t>(models[2]);

    for (int mi = 0; mi < 4; ++mi) {
        const uintptr_t model = models[mi];
        if (!valid_ptr(model)) continue;

        // Scan the model for a count + array-pointer pair that validates. The
        // count is a small int followed closely by a pointer to the array.
        for (uintptr_t off = 0x0; off <= 0x400; off += 4) {
            const int count = mem.read<int>(model + off);
            if (count < 8 || count > 40) continue;

            for (uintptr_t poff = off + 4; poff <= off + 0x20; poff += 4) {
                const uintptr_t arr = mem.read<uintptr_t>(model + poff);
                if (!valid_ptr(arr)) continue;
                if (!validate_set(mem, arr, count)) continue;

                // Validated. Read it out.
                out.count = count;
                for (int i = 0; i < count && i < 24; ++i) {
                    RawHitbox h{};
                    read_hitbox(mem, arr + (uintptr_t)i * 0x50, h);

                    // Local bounds are in bone space. Without a verified bone
                    // index we cannot transform yet, so store the local box and
                    // leave bone unresolved; the caller falls back to the mesh.
                    out.box[i].a    = h.min;
                    out.box[i].b    = h.max;
                    out.box[i].radius = h.radius;
                    out.box[i].group  = h.group;
                    out.box[i].bone   = -1;
                }

                g_ready.store(true);
                g_status = "validated (bone index unresolved)";
                g_attempts.fetch_add(1);
                out.ready = false;   // see note in the header: not used yet
                return false;
            }
        }
    }

    g_status = "no validated hitbox set found";
    g_attempts.fetch_add(1);
    return false;
}
