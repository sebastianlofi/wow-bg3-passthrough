// wdbloom (patch28, wowbg3_fxbg.inc.c): after BG3's composite, 1 in nob where the pixel is pure WoW: WoW's pixel, BG3's
// G-buffer still the bare placeholder (as wdbare), and BG3's forward buffer still what the feed wrote (no effect drawn
// over it). Under fxcal (pc.cal) every pixel the feed wrote counts. Writes no colour.
#version 450
layout(r32f, set = 0, binding = 0) uniform readonly image2D wmask;
layout(rgba32f, set = 0, binding = 1) uniform readonly image2D wfed;
layout(r32f, set = 0, binding = 2) uniform writeonly image2D nob;
layout(set = 0, binding = 3) uniform sampler2D b1;    // BG3's forward buffer, as the composite read it
layout(set = 0, binding = 4) uniform sampler2D alb;   // BG3's G-buffer albedo
layout(set = 0, binding = 5) uniform sampler2D surf;  // roughness, specular level, metalness, material byte
layout(rgba16f, set = 0, binding = 6) uniform writeonly image2D late;  // BG3's share of a pixel it drew over before its composite (wowbg3_post.frag goes on from it)
layout(rgba16f, set = 0, binding = 7) uniform writeonly image2D gain;  // under a decal: its change of WoW's ground a channel (wdpostcas)
layout(push_constant) uniform PC { uint cal; } pc;
layout(location = 0) out vec4 o;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    vec3 f = texelFetch(b1, q, 0).rgb;
    vec4 w = imageLoad(wfed, q);
    // B10G11R11 keeps 5 or 6 mantissa bits: what the feed wrote comes back within 3.2 %.
    bool same = w.a > 0.5 && all(lessThanEqual(abs(f - w.rgb), w.rgb * 0.04 + vec3(1e-6)));
    bool pure = same;
    if (pc.cal == 0u) {
        vec4 a = texelFetch(alb, q, 0), s = texelFetch(surf, q, 0);
        uint mat = uint(s.w * 255.0 + 0.5);
        bool bare = all(lessThan(abs(a.rgb - vec3(46.0 / 255.0)), vec3(0.5 / 255.0))) && s.x > 254.5 / 255.0 && s.y < 0.5 / 255.0 && s.z < 0.5 / 255.0 && (mat & 0xf7u) == 0u;
        pure = same && bare && imageLoad(wmask, q).r > 0.0;
    }
    // WoW's ground with something of BG3's on it: not pure, but still WoW's pixel under BG3's addition. Before, such a
    // pixel was BG3's composite alone, and an ability whose range decal covers the screen put BG3's relit picture of
    // WoW's ground back everywhere (a playtest report: "the fog comes back when I aim").
    // The mask holds, negative, what wdpostcas needs: -(1 + t * 255), t (0..1 in 255 steps) BG3's share of the pixel.
    //   * Something drawn over the ground (an effect, a preview: the G-buffer still bare, the forward buffer changed):
    //     t is the share of the pixel's light that BG3 added.
    //   * A decal over the placeholder (a surface, an ability's range): BG3's albedo there over the placeholder's grey
    //     (wowbg3_ground.frag). Each channel's ratio to the grey is the change the decal makes to a ground of that
    //     albedo, so WoW's colour times it is the decal on WoW's ground in WoW's own light: water darkens it, blood
    //     darkens and reddens it, a trace of either does little. That gain goes in `gain`; `late` stays free for what
    //     BG3 draws over the decal after its composite (wowbg3_ovl.comp), as over bare ground. A decal brighter
    //     than the ground by enough to hide it (grease, an ability's area) is BG3's own lit pixel by that much (t).
    //     The placeholder was black before, and a surface that darkens could not darken black: water was not seen at
    //     all (a playtest report: "the water is invisible on the ground"). Lightening WoW's colour by the decal's albedo
    //     over black (tried before) showed a pale patch under every faint decal, a trace of blood among them.
    float m = pure ? 1.0 : 0.0;
    vec4 early = vec4(0.0);
    if (!pure && pc.cal == 0u && w.a > 0.5 && imageLoad(wmask, q).r > 0.0) {
        vec3 d = abs(f - w.rgb) / max(max(f, w.rgb), vec3(1e-5));
        float t = clamp((max(d.r, max(d.g, d.b)) - 0.04) / 0.96, 0.0, 1.0), g = 0.0;
        const vec3 PH = vec3(46.0 / 255.0);
        vec3 A = texelFetch(alb, q, 0).rgb;
        if (any(greaterThan(abs(A - PH), vec3(0.5 / 255.0)))) {
            vec3 up = max(A - PH, vec3(0.0)) / (1.0 - PH);
            t = smoothstep(0.06, 0.30, max(up.r, max(up.g, up.b)));
            m = -(1.0 + floor(t * 255.0 + 0.5) + g);
            imageStore(gain, q, vec4(clamp(A / PH, vec3(0.0), 1.0 / PH), 1.0));
        } else {
            // Something drawn over the bare ground before the composite (a glow at a caster's feet, a bolt): what it
            // added is known a channel, what the feed wrote against what stands there now, as wowbg3_post.frag
            // keeps what BG3's effects pass adds later. The pixel stays WoW's own colour with that on it (the mask
            // says 0.3: wdpost and wdpostcas treat it as pure, the bloom keeps it). Before, such a pixel was
            // cross-faded to BG3's own picture of the ground: a darker ragged patch under every such effect.
            early = vec4(mix(-(1.0 - f / max(w.rgb, vec3(1e-6))), (f - w.rgb) / max(f, vec3(1e-6)), step(w.rgb, f)), 1.0);
            m = 0.3;
        }
    }
    imageStore(late, q, early);
    imageStore(nob, q, vec4(m));
    o = vec4(0.0);
}
