// --- src/hitbox.h ---
#pragma once
#include <cstdint>

#include "esp.h"     // Vec3
#include "memory.h"

// ===========================================================================
// REAL MODEL HITBOXES -- AN ATTEMPT, NOT A GUARANTEE
//
// WHAT IS VERIFIED
// The CHitBox field layout is taken from Valve's own schema dump
// (SteamTracking/GameTracking-CS2, modellib/CHitBox.h):
//
//   0x00 m_name            CUtlString
//   0x08 m_sSurfaceProperty CUtlString
//   0x10 m_sBoneName       CUtlString
//   0x18 m_vMinBounds      Vector
//   0x24 m_vMaxBounds      Vector
//   0x30 m_flShapeRadius   float32
//   0x34 m_nBoneNameHash   uint32
//   0x38 m_nGroupId        int32
//   0x3C m_nShapeType      uint8
//   0x3D m_bTranslationOnly bool
//   0x40 m_CRC             uint32
//   0x44 m_cRenderColor    Color
//   0x48 m_nHitBoxIndex    uint16
//   stride 0x50
//
// WHAT IS NOT VERIFIED
// The pointer path from an entity to the hitbox ARRAY. The definitions live in
// the model resource, reached from CModelState::m_hModel (a CStrongHandle at
// 0xA0), which is not a plain pointer -- it has to be resolved through the
// resource system, and no verified layout for that chain or for CModel's
// hitbox-set data was found.
//
// Community externals do not read hitbox definitions; the widely circulated
// "GetHitboxData()" returns m_modelState + 0x80, which is the BONE array, and
// its "GetHitboxPos" is just the parent bone position. So bone-based hit
// testing IS the external norm.
//
// HOW THIS MODULE HANDLES THAT
// It tries the model-resource path and then VALIDATES hard before accepting
// anything: a hitbox set must have a plausible count, small sane bounds with
// min <= max, a plausible shape type, and bone indices that resolve. If
// validation fails it reports not-ready and nothing uses it. A wrong guess
// therefore costs nothing; it cannot corrupt the triggerbot.
//
// The triggerbot keeps the bone mesh as its reliable path regardless.
// ===========================================================================

struct HitboxCapsule {
    Vec3  a{}, b{};   // world-space axis endpoints
    float radius = 0.0f;
    int   bone = -1;  // parent bone index, -1 if unresolved
    int   group = 0;  // hit group id
};

struct HitboxSetResult {
    bool ready = false;   // resolved AND passed validation
    int  count = 0;
    HitboxCapsule box[24];
};

void Hitbox_Init();

// Resolve hitboxes for one pawn. Returns true only when `out.ready`.
bool Hitbox_Get(const Memory& mem, uintptr_t pawn, HitboxSetResult& out);

// Human-readable status for the menu.
const char* Hitbox_Status();
int  Hitbox_Attempts();
