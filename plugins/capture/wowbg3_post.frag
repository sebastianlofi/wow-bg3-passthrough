// WOW-BG3 capture layer (wdpost, wowbg3_fxbg.inc.c): at the end of BG3's TAA pass, BG3's own TAA input (this frame's
// exposure-applied composite, no history) over the TAA output where the pixel is pure WoW ground. Pure (patch46): wdbloom's
// mask, made right after BG3's composite while the G-buffer is valid: the placeholder shows, BG3's G-buffer is the bare
// placeholder and the forward target still holds what the feed wrote. Everything else keeps BG3's TAA.
// wdpostdbg (patch45): green where it re-places instead.
#version 450
layout(r32f, set = 0, binding = 0) uniform readonly image2D nob;  // 1: pure WoW ground
layout(set = 0, binding = 5) uniform sampler2D cur;                // TAA's input
layout(location = 0) out vec4 o;
layout(push_constant) uniform PC { uint dbg; } pc;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (!(imageLoad(nob, q).r > 0.5)) discard;
    o = pc.dbg != 0u ? vec4(0.0, 8.0, 0.0, 1.0) : vec4(texelFetch(cur, q, 0).rgb, 1.0);
}
