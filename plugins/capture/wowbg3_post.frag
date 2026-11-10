// WOW-BG3 capture layer (wdpost, wowbg3_fxbg.inc.c): at the end of BG3's TAA pass, BG3's own TAA input (this frame's
// exposure-applied composite, no history) over the TAA output where the pixel is pure WoW ground. Pure (patch46): wdbloom's
// mask, made right after BG3's composite while the G-buffer is valid: the placeholder shows, BG3's G-buffer is the bare
// placeholder and the forward target still holds what the feed wrote. Everything else keeps BG3's TAA.
// A pure pixel BG3 drew nothing over after that (its effects pass: a spell, smoke, a glow) still holds what the feed
// wrote in BG3's forward target: the mask says 2 there, and wdpostcas shows WoW's own colour for it. Push constant bit 2 (word wdtable): never.
// wdpostdbg (patch45): green where it re-places instead.
#version 450
layout(r32f, set = 0, binding = 0) uniform image2D nob;            // 1: pure WoW ground; written 2: and untouched since the feed
layout(set = 0, binding = 1) uniform sampler2D fed;                // what the feed wrote
layout(set = 0, binding = 2) uniform sampler2D fwd;                // BG3's forward target, after its effects pass
layout(set = 0, binding = 5) uniform sampler2D cur;                // TAA's input
layout(location = 0) out vec4 o;
layout(push_constant) uniform PC { uint dbg; } pc;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (!(imageLoad(nob, q).r > 0.5)) discard;
    vec3 c = texelFetch(cur, q, 0).rgb;
    // Pure at the composite (wowbg3_blmask.frag: the forward buffer the composite read was what the feed wrote, the
    // G-buffer bare) is untouched: BG3's effects are in that buffer by then. This pass used to look at the forward
    // image again here, at the TAA, and call the pixel touched when it differed from the feed; but BG3 uses that image
    // for other things after the composite in some states (turn-based mode, an armed ability with a wide range), so
    // every pixel failed at once and BG3's own relit picture of WoW's ground came back over the whole view (a playtest
    // report: "the fog comes back when I aim, and in turn-based mode").
    imageStore(nob, q, vec4((pc.dbg & 2u) == 0u ? 2.0 : 1.0));
    o = (pc.dbg & 1u) != 0u ? vec4(0.0, 8.0, 0.0, 1.0) : vec4(c, 1.0);
}
