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
layout(rgba16f, set = 0, binding = 6) uniform image2D late;  // read: BG3's share of a pixel it drew over before its composite (wowbg3_blmask.frag); written: with what it drew after, a 1 where there is any
layout(location = 0) out vec4 o;
layout(push_constant) uniform PC { uint dbg; } pc;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    float n0 = imageLoad(nob, q).r;
    // 1: pure WoW ground at the composite; 0.3: WoW ground with something of BG3's drawn before the composite.
    if (!(n0 > 0.25)) { imageStore(late, q, vec4(0.0)); discard; }
    vec4 e = n0 < 0.5 ? imageLoad(late, q) : vec4(0.0);
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
    // pixel a channel: where it added light, the light's share of the pixel (1 - before / after: towards 1 a glow that
    // outshines the ground), and wdpostcas adds that share of BG3's displayed pixel to WoW's own colour; where it took
    // light away (smoke, a dark particle), minus the part taken (after / before - 1), and WoW's colour loses the same
    // part. Not a difference of displayed colours there: BG3's picture of the ground can be far brighter than WoW's
    // (its exposure during a bright cast), and taking that much off WoW's night ground left it black.
    vec4 l = vec4(0.0);
    if ((pc.dbg & 4u) != 0u) {
        vec3 b = texelFetch(cmp0, q, 0).rgb, a = texelFetch(cmp1, q, 0).rgb;
        // The same image twice: equal bit for bit where nothing was drawn.
        if (any(greaterThan(abs(a - b), max(a, b) * 0.02 + vec3(1e-6))))
            l = vec4(mix(-(1.0 - a / max(b, vec3(1e-6))), (a - b) / max(a, vec3(1e-6)), step(b, a)), 1.0);
    }
    if (e.a > 0.5) {
        // With what was drawn before the composite: what both let through, and the light either added.
        vec3 l1 = e.rgb, l2 = l.a > 0.5 ? l.rgb : vec3(0.0);
        vec3 thru = (1.0 + min(l1, vec3(0.0))) * (1.0 + min(l2, vec3(0.0)));
        vec3 add = 1.0 - (1.0 - max(l1, vec3(0.0))) * (1.0 - max(l2, vec3(0.0)));
        l = vec4(mix(thru - 1.0, add, step(vec3(0.005), add)), 1.0);
    }
    imageStore(late, q, l);
    // 2: pure and untouched by the bloom's cut; 0.4: WoW's colour with BG3's early addition, which keeps its bloom.
    imageStore(nob, q, vec4(n0 < 0.5 ? 0.4 : ((pc.dbg & 2u) == 0u ? 2.0 : 1.0)));
    o = (pc.dbg & 1u) != 0u ? vec4(0.0, 8.0, 0.0, 1.0) : vec4(c, 1.0);
}
