// wdbloom (patch28, wowbg3_fxbg.inc.c): after BG3's bloom prefilter, its output (s times smaller than the frame) is 0
// where any of its source pixels is pure WoW (nob): what its min of four taps gives with those pixels at 0.
#version 450
layout(r32f, set = 0, binding = 0) uniform readonly image2D nob;
layout(push_constant) uniform PC { uint sx, sy, w, h; } pc;
layout(location = 0) out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) * ivec2(pc.sx, pc.sy);
    float any = 0.0;
    for (uint y = 0u; y < pc.sy; y++)
        for (uint x = 0u; x < pc.sx; x++) {
            ivec2 q = min(p + ivec2(x, y), ivec2(pc.w, pc.h) - 1);
            any = max(any, imageLoad(nob, q).r);
        }
    if (any < 0.5) discard;
    o = vec4(0.0, 0.0, 0.0, 1.0);
}
