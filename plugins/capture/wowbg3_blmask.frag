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
    imageStore(nob, q, vec4(pure ? 1.0 : 0.0));
    o = vec4(0.0);
}
