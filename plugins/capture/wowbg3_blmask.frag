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
        bool bare = all(lessThan(a.rgb, vec3(0.5 / 255.0))) && s.x > 254.5 / 255.0 && s.y < 0.5 / 255.0 && s.z < 0.5 / 255.0 && (mat & 0xf7u) == 0u;
        pure = same && bare && imageLoad(wmask, q).r > 0.0;
    }
    // WoW's ground with something of BG3's on it: not pure, but still WoW's pixel under BG3's addition. Before, such a
    // pixel was BG3's composite alone, and an ability whose range decal covers the screen put BG3's relit picture of
    // WoW's ground back everywhere (a playtest report: "the fog comes back when I aim").
    // The mask holds, negative, what wdpostcas needs: -(1 + t * 255 + g), t (0..1 in 255 steps) BG3's share of the
    // pixel and g (0..0.99) a gain on WoW's own colour.
    //   * Something drawn over the ground (an effect, a preview: the G-buffer still bare, the forward buffer changed):
    //     t is the share of the pixel's light that BG3 added.
    //   * A decal lit over the placeholder (an ability's range, a surface): BG3 lights the decal's own colour there,
    //     and over its own ground that lightens the ground by the decal's share of the albedo. The placeholder's albedo
    //     is black, so BG3's result is the decal's colour alone under BG3's exposure: a grey lift that covers WoW's
    //     night ground like fog. WoW's colour gets the lightening the decal means instead (g: the decal's albedo over
    //     a ground albedo of 0.18), and a decal strong enough to hide the ground (t from its albedo) is BG3's pixel.
    float m = pure ? 1.0 : 0.0;
    if (!pure && pc.cal == 0u && w.a > 0.5 && imageLoad(wmask, q).r > 0.0) {
        vec3 d = abs(f - w.rgb) / max(max(f, w.rgb), vec3(1e-5));
        float t = clamp((max(d.r, max(d.g, d.b)) - 0.04) / 0.96, 0.0, 1.0), g = 0.0;
        vec4 a = texelFetch(alb, q, 0);
        float al = max(a.r, max(a.g, a.b));
        if (al >= 0.5 / 255.0) {
            t = smoothstep(0.06, 0.30, al);
            g = min(dot(a.rgb, vec3(0.2126, 0.7152, 0.0722)) / 0.18, 0.99);
        }
        m = -(1.0 + floor(t * 255.0 + 0.5) + g);
    }
    imageStore(nob, q, vec4(m));
    o = vec4(0.0);
}
