// --- src/esp.cpp ---
#include "esp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

bool valid_ptr(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFF0000ULL && (p % 4) == 0;
}

bool sane_vec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 100000.0f && std::fabs(v.y) < 100000.0f &&
           std::fabs(v.z) < 100000.0f &&
           !(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f);
}

void lerp3(Vec3& out, const Vec3& a, const Vec3& b, float f) {
    out.x = a.x + (b.x - a.x) * f;
    out.y = a.y + (b.y - a.y) * f;
    out.z = a.z + (b.z - a.z) * f;
}

Vec3 read_origin(const Memory& mem, uintptr_t ent) {
    Vec3 v{};
    mem.read_bytes(ent + offsets::m_vOldOrigin, &v, sizeof(v));
    return v;
}

// ── entity access ────────────────────────────────────────────────────────

struct EntHead { int health = 0; int team = 0; };

// One 0x100 window covers health and team. Falls back to two separate reads --
// the path the radar uses, so a refused bulk read can't empty the list.
EntHead read_ent_head(const Memory& mem, uintptr_t ent) {
    EntHead o;
    uint8_t buf[0x100];
    if (mem.read_bytes(ent + 0x340, buf, sizeof(buf))) {
        std::memcpy(&o.health, buf + (offsets::m_iHealth - 0x340), 4);
        o.team = buf[offsets::m_iTeamNum - 0x340];
        return o;
    }
    o.health = mem.read<int>(ent + offsets::m_iHealth);
    o.team   = mem.read<uint8_t>(ent + offsets::m_iTeamNum);
    return o;
}

// ── entity-list enumeration ──────────────────────────────────────────────
// Each read is a WHOLE NUMBER OF SLOTS. Reading fixed 16 KB pieces used to step
// off the slot grid (16384 / 120 = 136.5), so every piece after the first read
// from 64 bytes inside a slot and returned noise.
void enumerate_slots(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                     uintptr_t stride, std::vector<uintptr_t>& out) {
    out.clear();
    if (!stride) return;

    constexpr size_t kSlotsPerRead = 128;
    const size_t piece_bytes = kSlotsPerRead * stride;

    static std::vector<uint8_t> buf;
    if (buf.size() < piece_bytes) buf.resize(piece_bytes);

    for (int ch = 0; ch < offsets::kChunks; ++ch) {
        const uintptr_t base = mem.read<uintptr_t>(el + chunk_off + 8 * ch);
        if (!valid_ptr(base)) continue;

        for (size_t s0 = 0; s0 < static_cast<size_t>(offsets::kSlots);
             s0 += kSlotsPerRead) {
            const size_t n = (std::min)(kSlotsPerRead,
                             static_cast<size_t>(offsets::kSlots) - s0);
            if (!mem.read_bytes(base + s0 * stride, buf.data(), n * stride))
                break;
            for (size_t k = 0; k < n; ++k) {
                uintptr_t e = 0;
                std::memcpy(&e, buf.data() + k * stride, 8);
                if (valid_ptr(e)) out.push_back(e);
            }
        }
    }
}

uintptr_t resolve_handle(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                         uintptr_t stride, uint32_t handle) {
    const uint32_t idx = handle & 0x7FFF;
    if (!idx) return 0;
    const uintptr_t chunk =
        mem.read<uintptr_t>(el + chunk_off + 8 * (idx >> 9));
    if (!valid_ptr(chunk)) return 0;
    return mem.read<uintptr_t>(chunk + stride * (idx & 0x1FF));
}

void sanitize(char* dst, size_t cap, const char* src, size_t src_len) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < cap && i < src_len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        if (ch == 0) break;
        if (ch >= 32 && ch < 127) dst[j++] = static_cast<char>(ch);
    }
    dst[j] = '\0';
}

// ── weapon names from item definition index ──────────────────────────────
// This is the radar's PRIMARY path: definition index, self-tested at startup.
const char* weapon_name(int id) {
    switch (id) {
        case 1:  return "Deagle";     case 2:  return "Dual Berettas";
        case 3:  return "Five-SeveN"; case 4:  return "Glock-18";
        case 7:  return "AK-47";      case 8:  return "AUG";
        case 9:  return "AWP";        case 10: return "FAMAS";
        case 11: return "G3SG1";      case 13: return "Galil AR";
        case 14: return "M249";       case 16: return "M4A4";
        case 17: return "MAC-10";     case 19: return "P90";
        case 23: return "MP5-SD";     case 24: return "UMP-45";
        case 25: return "XM1014";     case 26: return "PP-Bizon";
        case 27: return "MAG-7";      case 28: return "Negev";
        case 29: return "Sawed-Off";  case 30: return "Tec-9";
        case 31: return "Zeus x27";   case 32: return "P2000";
        case 33: return "MP7";        case 34: return "MP9";
        case 35: return "Nova";       case 36: return "P250";
        case 38: return "SCAR-20";    case 39: return "SG 553";
        case 40: return "SSG 08";     case 43: return "Flashbang";
        case 44: return "HE Grenade"; case 45: return "Smoke";
        case 46: return "Molotov";    case 47: return "Decoy";
        case 48: return "Incendiary"; case 49: return "C4";
        case 57: return "Health Shot";
        case 60: return "M4A1-S";     case 61: return "USP-S";
        case 63: return "CZ75-Auto";  case 64: return "R8 Revolver";
        default: break;
    }
    if (id >= 500 && id <= 600) return "Knife";
    return nullptr;
}

// designer-name fallback: entity + 0x10 -> level1, level1 + 0x20 -> char*
bool read_designer_name(const Memory& mem, uintptr_t ent, char* out, size_t cap) {
    out[0] = '\0';
    const uintptr_t lvl1 = mem.read<uintptr_t>(ent + offsets::m_designerLvl1);
    if (!valid_ptr(lvl1)) return false;
    const uintptr_t np = mem.read<uintptr_t>(lvl1 + offsets::m_designerPtr);
    if (!valid_ptr(np)) return false;

    char raw[48] = {};
    if (!mem.read_bytes(np, raw, 40)) return false;
    raw[39] = '\0';

    const char* s = raw;
    if (std::strncmp(s, "weapon_", 7) == 0) s += 7;
    sanitize(out, cap, s, std::strlen(s));
    return out[0] != '\0';
}

// The exact read the radar performs for a definition index.
uint16_t item_def_index(const Memory& mem, uintptr_t weapon) {
    return mem.read<uint16_t>(weapon + offsets::m_AttributeManager +
                              offsets::m_Item +
                              offsets::m_iItemDefinitionIndex);
}

// Self-test the def-index chain against OUR OWN active weapon, exactly like the
// radar's ValidateDefIdxChain. If it returns a sane index for us, the chain is
// good for everyone; if not, weapons fall back to designer names.
bool validate_defidx_chain(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                           uintptr_t stride, uintptr_t local_pawn, int& out_id) {
    out_id = 0;
    const uintptr_t ws = mem.read<uintptr_t>(local_pawn + offsets::m_pWeaponServices);
    if (!valid_ptr(ws)) return false;

    const uint32_t h = mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
    if (!h || h == 0xFFFFFFFFu) return false;

    const uintptr_t w = resolve_handle(mem, el, chunk_off, stride, h);
    if (!valid_ptr(w)) return false;

    out_id = item_def_index(mem, w);
    return out_id >= 1 && out_id <= 600;
}

// ── planted C4 ───────────────────────────────────────────────────────────
bool looks_like_entity(const Memory& mem, uintptr_t ent) {
    if (!valid_ptr(ent)) return false;
    const uintptr_t gsn = mem.read<uintptr_t>(ent + offsets::m_pGameSceneNode);
    return valid_ptr(gsn);
}

// dwPlantedC4 may BE the entity or may point at one, and which it is differs
// between builds -- so try both, exactly like the radar does.
bool read_planted_c4(const Memory& mem, uintptr_t client_base, Vec3& out) {
    const uintptr_t p = mem.read<uintptr_t>(client_base + offsets::dwPlantedC4);
    if (!valid_ptr(p)) return false;

    uintptr_t ent = 0;
    if (looks_like_entity(mem, p)) {
        ent = p;
    } else {
        const uintptr_t inner = mem.read<uintptr_t>(p);
        if (looks_like_entity(mem, inner)) ent = inner;
    }
    if (!ent) return false;

    const uintptr_t node = mem.read<uintptr_t>(ent + offsets::m_pGameSceneNode);
    if (!valid_ptr(node)) return false;

    out.x = mem.read<float>(node + offsets::m_vecAbsOrigin);
    out.y = mem.read<float>(node + offsets::m_vecAbsOrigin + 4);
    out.z = mem.read<float>(node + offsets::m_vecAbsOrigin + 8);
    return sane_vec(out);
}

// ── skeleton ─────────────────────────────────────────────────────────────
constexpr float kHeadPad = 6.0f;
constexpr float kFallbackHeadZ = 64.0f;

struct BoneEntry { Vec3 pos; float pad[5]; };
static_assert(sizeof(BoneEntry) == offsets::m_boneStride,
              "CS2 bone stride must be 0x20");
struct TestPawn { uintptr_t pawn; Vec3 origin; };
struct BoneChain { uintptr_t node = 0, arr = 0; bool valid = false; };

int skeleton_points(const Memory& mem, uintptr_t arr, const Vec3& origin) {
    BoneEntry b[offsets::kBoneSlots];
    if (!mem.read_bytes(arr, b, sizeof(b))) return 0;

    int good = 0;
    for (int i = 0; i < offsets::kBoneSlots; ++i) {
        const Vec3& p = b[i].pos;
        if (!sane_vec(p)) continue;
        const float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
        if (std::fabs(dx) > 110.0f || std::fabs(dy) > 110.0f) continue;
        if (dz < -40.0f || dz > 150.0f) continue;
        ++good;
    }
    return good;
}

BoneChain probe_bones(const Memory& mem, const std::vector<TestPawn>& test) {
    BoneChain best;
    if (test.empty()) return best;

    int best_score = 0;
    const int need = (test.size() >= 2) ? 2 : 1;

    std::vector<uint8_t> node_buf(offsets::kNodeScanBytes);

    for (uintptr_t no = 0x8; no + 8 <= offsets::kPawnScanBytes; no += 8) {
        const uintptr_t node = mem.read<uintptr_t>(test[0].pawn + no);
        if (!valid_ptr(node)) continue;
        if (!mem.read_bytes(node, node_buf.data(), node_buf.size())) continue;

        for (uintptr_t ao = 0x8; ao + 8 <= offsets::kNodeScanBytes; ao += 8) {
            uintptr_t arr = 0;
            std::memcpy(&arr, node_buf.data() + ao, 8);
            if (!valid_ptr(arr)) continue;
            if (skeleton_points(mem, arr, test[0].origin) < 20) continue;

            int score = 1;
            for (size_t t = 1; t < test.size(); ++t) {
                const uintptr_t n = mem.read<uintptr_t>(test[t].pawn + no);
                if (!valid_ptr(n)) break;
                const uintptr_t a = mem.read<uintptr_t>(n + ao);
                if (!valid_ptr(a)) break;
                if (skeleton_points(mem, a, test[t].origin) < 20) break;
                ++score;
            }

            if (score > best_score) {
                best_score = score;
                best.node = no;
                best.arr  = ao;
            }
        }
    }

    best.valid = best_score >= need;
    return best;
}

bool read_bones(const Memory& mem, uintptr_t pawn, const Vec3& origin,
                const BoneChain& chain,
                std::array<Vec3, BONE_COUNT>& out,
                std::array<bool, BONE_COUNT>& ok) {
    out.fill(Vec3{});
    ok.fill(false);
    if (!chain.valid || !pawn) return false;

    const uintptr_t node = mem.read<uintptr_t>(pawn + chain.node);
    if (!valid_ptr(node)) return false;
    const uintptr_t arr = mem.read<uintptr_t>(node + chain.arr);
    if (!valid_ptr(arr)) return false;

    BoneEntry raw[BONE_COUNT];
    if (!mem.read_bytes(arr, raw, sizeof(raw))) return false;

    int n = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        const Vec3& p = raw[i].pos;
        if (!sane_vec(p)) continue;
        const float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
        if (std::fabs(dx) > 60.0f || std::fabs(dy) > 60.0f) continue;
        if (dz < -25.0f || dz > 90.0f) continue;
        out[i] = p;
        ok[i]  = true;
        ++n;
    }
    return n >= 6;
}

} // namespace

bool ESP::world_to_screen(const Vec3& world, Vec2& screen,
                          const Matrix4x4& vm, int screen_w, int screen_h) {
    const float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
                  + vm.m[3][2] * world.z + vm.m[3][3];
    if (w < 0.001f) return false;

    const float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
                  + vm.m[0][2] * world.z + vm.m[0][3];
    const float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
                  + vm.m[1][2] * world.z + vm.m[1][3];

    const float inv = 1.0f / w;
    screen.x = (screen_w * 0.5f) + (x * inv) * (screen_w * 0.5f);
    screen.y = (screen_h * 0.5f) - (y * inv) * (screen_h * 0.5f);
    return true;
}

// ── reader thread ────────────────────────────────────────────────────────
void ESP::update_world(const Memory& mem, uintptr_t client_base) {
    const double now = now_ms();

    const uintptr_t el =
        mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    const uintptr_t local_pawn =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);

    if (!el || !local_pawn) {
        std::lock_guard<std::mutex> lk(mtx);
        tracks_.clear();
        bomb_active_ = false;
        players_alive = 0;
        return;
    }

    const Vec3 local_origin = read_origin(mem, local_pawn);

    static std::vector<uintptr_t> slots;
    static std::vector<uintptr_t> pawns;
    static double slots_at = -1e9;
    static double info_at  = -1e9;
    static double c4_at    = -1e9;

    // ── refresh the pawn list ~10x/sec, auto-detecting the list layout ────
    if (now - slots_at > 100.0) {
        slots_at = now;

        struct Layout { uintptr_t off, stride; };
        static const Layout kLayouts[] = {
            {0x10, 120}, {0x10, 112}, {0x08, 120}, {0x08, 112},
        };

        int best = -1;
        std::vector<uintptr_t> best_all, best_pawns;
        for (const Layout& L : kLayouts) {
            std::vector<uintptr_t> all;
            enumerate_slots(mem, el, L.off, L.stride, all);

            std::vector<uintptr_t> got;
            for (uintptr_t e : all) {
                if (e == local_pawn) continue;          // never box ourselves
                const EntHead h = read_ent_head(mem, e);
                if (h.health <= 0 || h.health > 150) continue;
                if (h.team != 2 && h.team != 3) continue;
                got.push_back(e);
            }

            if (static_cast<int>(got.size()) > best) {
                best = static_cast<int>(got.size());
                c_.chunk_off   = L.off;
                c_.slot_stride = L.stride;
                best_all   = std::move(all);
                best_pawns = std::move(got);
            }
        }
        if (best > 0) {
            slots = std::move(best_all);
            pawns = std::move(best_pawns);
        } else {
            pawns.clear();
        }
        diag_slots = static_cast<int>(slots.size());
    }

    // ── item-def chain self-test (once, then remembered) ─────────────────
    if (!c_.defidx_checked && !pawns.empty()) {
        c_.defidx_checked = true;
        c_.defidx_ok = validate_defidx_chain(mem, el, c_.chunk_off,
                                             c_.slot_stride, local_pawn,
                                             diag_defidx);
    }

    // ── bone chain, resolved once and re-probed if it goes stale ─────────
    if (!c_.bones_ok || c_.bone_fail > 250) {
        if (c_.probe_cd > 0) {
            --c_.probe_cd;
        } else {
            std::vector<TestPawn> test;
            if (sane_vec(local_origin))
                test.push_back({ local_pawn, local_origin });
            for (uintptr_t p : pawns) {
                if (test.size() >= 4) break;
                test.push_back({ p, read_origin(mem, p) });
            }
            const BoneChain bc = probe_bones(mem, test);
            if (bc.valid) {
                c_.bone_node = bc.node;
                c_.bone_arr  = bc.arr;
                c_.bones_ok  = true;
                c_.bone_fail = 0;
            }
            c_.probe_cd = 125;   // ~1 s at an 8 ms tick
        }
    }

    // ── find the weapon_c4 entity, then discover who owns it ─────────────
    // The radar's method, and the reason my old approach failed: the carrier
    // is found from the C4 entity's OWNER handle, not from the player's
    // active weapon. Once discovered, the offset is cached (2 reads/frame).
    uintptr_t c4_ent  = 0;
    uintptr_t carrier = 0;

    if (!pawns.empty() && now - c4_at > 500.0) {
        c4_at = now;

        // Fast path: re-check the slot the C4 was in last time.
        if (c_.c4_chunk >= 0) {
            const uintptr_t cp = mem.read<uintptr_t>(
                el + c_.chunk_off + 8 * c_.c4_chunk);
            if (valid_ptr(cp)) {
                const uintptr_t e =
                    mem.read<uintptr_t>(cp + c_.slot_stride * c_.c4_slot);
                if (valid_ptr(e) && item_def_index(mem, e) == offsets::kItemDefC4)
                    c4_ent = e;
            }
            if (!c4_ent) { c_.c4_chunk = -1; c_.c4_slot = -1; }
        }

        // Full scan when the cache is cold. Only the def-index read is needed,
        // and only when the chain passed its self-test.
        if (!c4_ent && c_.defidx_ok) {
            for (int ch = 0; ch < offsets::kChunks && !c4_ent; ++ch) {
                const uintptr_t cp =
                    mem.read<uintptr_t>(el + c_.chunk_off + 8 * ch);
                if (!valid_ptr(cp)) continue;
                for (int i = 1; i < offsets::kSlots; ++i) {
                    const uintptr_t e =
                        mem.read<uintptr_t>(cp + c_.slot_stride * i);
                    if (!valid_ptr(e)) continue;
                    if (item_def_index(mem, e) != offsets::kItemDefC4) continue;
                    c4_ent = e;
                    c_.c4_chunk = ch;
                    c_.c4_slot  = i;
                    break;
                }
            }
        }

        // Owner discovery: scan the C4 entity for a handle that resolves to one
        // of the pawns we already know are real. Throttled to once per second
        // because it is the expensive part.
        if (c4_ent && !c_.carrier_ok && now - c_.last_discover > 1000.0) {
            c_.last_discover = now;
            for (uintptr_t off = 0x40; off <= 0x1400; off += 4) {
                const uint32_t h = mem.read<uint32_t>(c4_ent + off);
                if (!h || h == 0xFFFFFFFFu || (h & 0x7FFF) == 0) continue;
                const uintptr_t owner =
                    resolve_handle(mem, el, c_.chunk_off, c_.slot_stride, h);
                if (!owner) continue;
                if (std::find(pawns.begin(), pawns.end(), owner) != pawns.end()) {
                    c_.carrier_off = off;
                    c_.carrier_ok  = true;
                    break;
                }
            }
        }

        if (c4_ent && c_.carrier_ok) {
            const uint32_t h = mem.read<uint32_t>(c4_ent + c_.carrier_off);
            if (h && h != 0xFFFFFFFFu)
                carrier = resolve_handle(mem, el, c_.chunk_off,
                                         c_.slot_stride, h);
        }
    }
    diag_carrier = static_cast<int>(carrier);

    // ── per-tick sample of every live pawn ───────────────────────────────
    std::vector<Track> fresh;
    fresh.reserve(pawns.size());
    int bone_hits = 0;

    for (uintptr_t p : pawns) {
        const EntHead h = read_ent_head(mem, p);
        if (h.health <= 0 || h.health > 150) continue;

        const Vec3 origin = read_origin(mem, p);
        if (!sane_vec(origin)) continue;

        Snap s;
        s.t = now;
        s.origin = origin;
        s.has_bones = read_bones(mem, p, origin,
                                 BoneChain{ c_.bone_node, c_.bone_arr,
                                            c_.bones_ok },
                                 s.bones, s.bone_ok);
        if (s.has_bones) ++bone_hits;
        s.head = (s.has_bones && s.bone_ok[BONE_HEAD])
                     ? s.bones[BONE_HEAD]
                     : Vec3{ origin.x, origin.y, origin.z + kFallbackHeadZ };

        // Reuse the existing track so its history, name and weapon survive.
        Track* dst = nullptr;
        for (Track& t : tracks_) {
            if (t.pawn == p) { dst = &t; break; }
        }
        if (!dst) {
            tracks_.push_back(Track{});
            dst = &tracks_.back();
            dst->pawn = p;
        }

        const float dx = origin.x - local_origin.x;
        const float dy = origin.y - local_origin.y;
        const float dz = origin.z - local_origin.z;

        Track moved = *dst;
        moved.health    = h.health;
        moved.team      = h.team;
        moved.distance  = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
        moved.has_bomb  = (carrier != 0 && p == carrier);
        moved.push(s);
        fresh.push_back(std::move(moved));
    }

    if (c_.bones_ok && !pawns.empty() && bone_hits == 0) ++c_.bone_fail;
    else                                                c_.bone_fail = 0;

    // ── names + weapons, ~3x/sec ─────────────────────────────────────────
    if (now - info_at > 300.0) {
        info_at = now;

        for (Track& t : fresh) {
            // name: pawn -> controller (schema-supported direction)
            const uint32_t hc = mem.read<uint32_t>(t.pawn + offsets::m_hController);
            if (hc && hc != 0xFFFFFFFFu) {
                const uintptr_t ctrl = resolve_handle(mem, el, c_.chunk_off,
                                                      c_.slot_stride, hc);
                if (valid_ptr(ctrl)) {
                    char raw[128] = {};
                    if (mem.read_bytes(ctrl + offsets::m_iszPlayerName,
                                       raw, sizeof(raw))) {
                        raw[127] = '\0';
                        sanitize(t.name, sizeof(t.name), raw, sizeof(raw));
                    }
                }
            }

            // weapon: PRIMARY = item-definition index (chain self-tested);
            // FALLBACK = designer-name string, same as the radar.
            const uintptr_t ws =
                mem.read<uintptr_t>(t.pawn + offsets::m_pWeaponServices);
            if (valid_ptr(ws)) {
                const uint32_t hw =
                    mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
                if (hw && hw != 0xFFFFFFFFu) {
                    const uintptr_t w = resolve_handle(mem, el, c_.chunk_off,
                                                       c_.slot_stride, hw);
                    if (valid_ptr(w)) {
                        t.weapon[0] = '\0';
                        if (c_.defidx_ok) {
                            const int id = item_def_index(mem, w);
                            const char* nm = weapon_name(id);
                            if (nm) std::snprintf(t.weapon, sizeof(t.weapon),
                                                  "%s", nm);
                        }
                        if (!t.weapon[0]) {
                            char dn[24] = {};
                            if (read_designer_name(mem, w, dn, sizeof(dn))) {
                                if (std::strcmp(dn, "c4") == 0)
                                    std::snprintf(t.weapon, sizeof(t.weapon), "C4");
                                else
                                    std::snprintf(t.weapon, sizeof(t.weapon),
                                                  "%s", dn);
                            }
                        }
                    }
                }
            }
        }
    }

    // ── planted C4 ───────────────────────────────────────────────────────
    Vec3 bomb{};
    const bool bomb_active = read_planted_c4(mem, client_base, bomb);

    std::lock_guard<std::mutex> lk(mtx);
    tracks_       = std::move(fresh);
    bomb_active_  = bomb_active;
    bomb_origin_  = bomb;
    players_alive = static_cast<int>(tracks_.size());
}

// ── render thread ────────────────────────────────────────────────────────
namespace {

// Pick the two real frames bracketing `rt` and interpolate between them.
bool sample_hist(const Track& t, double rt, Snap& out) {
    if (t.count == 0) return false;

    auto at = [&](int back) {
        const int i = ((t.head - 1 - back) % Track::kHist + Track::kHist)
                      % Track::kHist;
        return t.hist[i];
    };

    const Snap newest = at(0);
    if (t.count == 1 || rt >= newest.t) { out = newest; return true; }

    const Snap oldest = at(t.count - 1);
    if (rt <= oldest.t) { out = oldest; return true; }

    for (int i = 0; i + 1 < t.count; ++i) {
        const Snap a = at(i);       // newer
        const Snap b = at(i + 1);   // older
        if (rt >= b.t && rt <= a.t) {
            const double span = a.t - b.t;
            const float f = span > 0.0001
                          ? static_cast<float>((rt - b.t) / span) : 0.0f;
            out = b;
            out.t = rt;
            lerp3(out.origin, b.origin, a.origin, f);
            lerp3(out.head,   b.head,   a.head,   f);
            out.has_bones = false;
            for (int k = 0; k < BONE_COUNT; ++k) {
                out.bone_ok[k] = b.bone_ok[k] && a.bone_ok[k];
                if (out.bone_ok[k]) {
                    lerp3(out.bones[k], b.bones[k], a.bones[k], f);
                    out.has_bones = true;
                }
            }
            return true;
        }
    }
    out = newest;
    return true;
}

// A teleport/respawn must snap, never smear across the map.
bool jumped(const Snap& a, const Snap& b) {
    const float dx = a.origin.x - b.origin.x;
    const float dy = a.origin.y - b.origin.y;
    const float dz = a.origin.z - b.origin.z;
    return dx * dx + dy * dy + dz * dz > 500.0f * 500.0f;
}

} // namespace

std::vector<PlayerESP> ESP::project(const Memory& mem, uintptr_t client_base,
                                    int screen_w, int screen_h) {
    const Matrix4x4 vm = mem.read<Matrix4x4>(
        client_base + offsets::dwViewMatrix);

    const double rt = now_ms() - static_cast<double>(interp_delay_ms);

    std::lock_guard<std::mutex> lk(mtx);

    std::vector<PlayerESP> out;
    out.reserve(tracks_.size());

    for (const Track& t : tracks_) {
        Snap s{};
        if (!sample_hist(t, rt, s)) continue;

        const Snap newest = t.hist[(t.head - 1 + Track::kHist) % Track::kHist];
        if (jumped(s, newest)) s = newest;

        const Vec3 box_top{ s.head.x, s.head.y, s.head.z + kHeadPad };

        PlayerESP e;
        if (!world_to_screen(s.head,   e.screen_head, vm, screen_w, screen_h)) continue;
        if (!world_to_screen(box_top,  e.screen_top,  vm, screen_w, screen_h)) continue;
        if (!world_to_screen(s.origin, e.screen_feet, vm, screen_w, screen_h)) continue;

        e.box_h = e.screen_feet.y - e.screen_top.y;
        if (e.box_h < 5.0f) continue;

        e.box_w     = e.box_h * 0.45f;
        e.health    = t.health;
        e.team      = t.team;
        e.distance  = t.distance;
        e.has_bones = s.has_bones;
        e.has_bomb  = t.has_bomb;
        std::snprintf(e.name,   sizeof(e.name),   "%s", t.name);
        std::snprintf(e.weapon, sizeof(e.weapon), "%s", t.weapon);

        if (s.has_bones) {
            for (int i = 0; i < BONE_COUNT; ++i) {
                if (!s.bone_ok[i]) continue;
                Vec2 sp{};
                if (!world_to_screen(s.bones[i], sp, vm, screen_w, screen_h))
                    continue;
                e.bones[i]   = sp;
                e.bone_ok[i] = true;
            }
        }

        out.push_back(e);
    }

    return out;
}

BombESP ESP::project_bomb(const Memory& mem, uintptr_t client_base,
                          int screen_w, int screen_h) {
    BombESP b;
    std::lock_guard<std::mutex> lk(mtx);
    if (!bomb_active_) return b;

    const Matrix4x4 vm = mem.read<Matrix4x4>(
        client_base + offsets::dwViewMatrix);

    const uintptr_t local_pawn =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    const Vec3 lo = read_origin(mem, local_pawn);

    if (!world_to_screen(bomb_origin_, b.screen, vm, screen_w, screen_h))
        return b;

    const Vec3 top{ bomb_origin_.x, bomb_origin_.y, bomb_origin_.z + 22.0f };
    if (!world_to_screen(top, b.screen_top, vm, screen_w, screen_h))
        return b;

    const float dx = bomb_origin_.x - lo.x;
    const float dy = bomb_origin_.y - lo.y;
    const float dz = bomb_origin_.z - lo.z;
    b.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
    b.active   = true;
    return b;
}
