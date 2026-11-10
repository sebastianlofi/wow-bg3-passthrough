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
layout(set = 0, binding = 3) uniform sampler2D cmp0;               // BG3's composite before its effects pass (bit 4: this frame's)
layout(set = 0, binding = 4) uniform sampler2D cmp1;               // and after it
layout(set = 0, binding = 5) uniform sampler2D cur;                // TAA's input
layout(rgba16f, set = 0, binding = 6) uniform writeonly image2D late;  // what BG3 drew over a pure pixel after its composite: its share of the pixel a channel, a 1 where it has one
layout(location = 0) out vec4 o;
layout(push_constant) uniform PC { uint dbg; } pc;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (!(imageLoad(nob, q).r > 0.5)) { imageStore(late, q, vec4(0.0)); discard; }
    vec3 c = texelFetch(cur, q, 0).rgb;
    // Pure at the composite (wowbg3_blmask.frag: the forward buffer the composite read was what the feed wrote, the
    // G-buffer bare). This pass used to look at the forward image again here, at the TAA, and call the pixel touched
    // when it differed from the feed; but BG3 uses that image for other things after the composite in some states
    // (turn-based mode, an armed ability with a wide range), so every pixel failed at once and BG3's own relit picture
    // of WoW's ground came back over the whole view (a playtest report: "the fog comes back when I aim, and in
    // turn-based mode").
    // BG3 draws more after its composite, straight into the composite's image (its effects pass): the aura of a
    // character aiming, an ability's area on the ground, the wisps and glow of a spell. That image is copied before and
    // after the pass: where the two differ, the difference is what BG3 drew over WoW's ground, kept as its share of the
    // pixel a channel (1 - before / after: 0 nothing, towards 1 a glow that outshines the ground, negative where it
    // darkened). wdpostcas adds that share of BG3's displayed pixel to WoW's own colour.
    vec4 l = vec4(0.0);
    if ((pc.dbg & 4u) != 0u) {
        vec3 b = texelFetch(cmp0, q, 0).rgb, a = texelFetch(cmp1, q, 0).rgb;
        // The same image twice: equal bit for bit where nothing was drawn.
        if (any(greaterThan(abs(a - b), max(a, b) * 0.02 + vec3(1e-6)))) l = vec4(clamp((a - b) / max(a, vec3(1e-6)), vec3(-8.0), vec3(1.0)), 1.0);
    }
    imageStore(late, q, l);
    imageStore(nob, q, vec4((pc.dbg & 2u) == 0u ? 2.0 : 1.0));
    o = (pc.dbg & 1u) != 0u ? vec4(0.0, 8.0, 0.0, 1.0) : vec4(c, 1.0);
}
