// The stand-in under WoW's model (world mode; wowbg3_plugin.cpp's hook_all, WowCursor.lua, StandInAim.lua).
//
// WoW draws a mirrored mob or NPC with its own model; BG3's character for it, the stand-in, is often smaller
// (a Thunder Lizard's is BG3's wolf) and BG3's cursor picks only the stand-in's own body. So the pointer on
// WoW's model picked ground: a click walked, a spell aimed at a character found none. While WoW reports such a
// unit under the pointer and BG3's cursor query has no character first, the query is cast again from the same
// eye toward the stand-in's body; when that one's first hit is a living character beside the stand-in's feet,
// its hits are the query's. BG3 then picks its own stand-in and does the rest itself: its cursor for it, and
// whatever its click does there (an attack, a spell or a throw aimed at it, a shove, talk, pickpocket), by its
// own rules. Pure: no game types here, so tools/test_standin_aim.py compiles it alone.
#pragma once

#include <cmath>
#include <cstdint>

namespace wowbg3_aim {

struct Hit {
    float Pos[3];
    float Dist;
    std::uint32_t Groups;
};

constexpr std::uint32_t kCharacter = 0x200;  // BG3's physics group of a living character
constexpr std::uint32_t kDead = 0x4000;      // and of a dead one (kKeepGroups in the plugin)
// Heights over the stand-in's feet the query is cast toward, in order: the body's middle first (as the
// harness's clicks), then lower for a small one (a rat, a wolf), then higher.
constexpr float kLifts[] = {1.0f, 0.5f, 1.5f, 0.25f};
constexpr int kLiftCount = (int)(sizeof(kLifts) / sizeof(kLifts[0]));
// How far from the stand-in's feet the character hit may lie: across, and over them.
constexpr float kReachM = 1.5f;
constexpr float kBelowM = 0.5f, kAboveM = 3.0f;

// The nearest of n hits (BG3 keeps a character only as the query's first hit), -1 for none.
inline int nearest(Hit const* h, int n) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
        if (best < 0 || h[i].Dist < h[best].Dist) best = i;
    }
    return best;
}

// BG3 picks a character from these hits already (a living one or a body): nothing to do.
inline bool picks_character(Hit const* h, int n) {
    const int i = nearest(h, n);
    return i >= 0 && (h[i].Groups & (kCharacter | kDead)) != 0;
}

// The redirected query's hits pick the stand-in standing at feet: the first hit is a living character beside
// its feet. (The physics hit names no entity; another character that close in front of it is not told apart.)
inline bool hits_stand_in(Hit const* h, int n, float const feet[3]) {
    const int i = nearest(h, n);
    if (i < 0 || (h[i].Groups & kCharacter) == 0) return false;
    const float dx = h[i].Pos[0] - feet[0], dz = h[i].Pos[2] - feet[2], dy = h[i].Pos[1] - feet[1];
    return dx * dx + dz * dz <= kReachM * kReachM && dy >= -kBelowM && dy <= kAboveM;
}

// The redirected segment: from the query's start toward lift metres over the feet, as long as the query's.
// False when the point is the start itself.
inline bool aim_end(float const a[3], float const b[3], float const feet[3], float lift, float out[3]) {
    const float t[3] = {feet[0], feet[1] + lift, feet[2]};
    float d[3], len = 0, dl = 0;
    for (int k = 0; k < 3; ++k) {
        d[k] = t[k] - a[k];
        dl += d[k] * d[k];
        len += (b[k] - a[k]) * (b[k] - a[k]);
    }
    dl = std::sqrt(dl);
    len = std::sqrt(len);
    if (!(dl > 1e-3f) || !(len > 1e-3f)) return false;
    for (int k = 0; k < 3; ++k) out[k] = a[k] + d[k] / dl * len;
    return true;
}

}  // namespace wowbg3_aim
