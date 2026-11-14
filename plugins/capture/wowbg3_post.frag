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
layout(set = 0, binding = 3) uniform sampler2D cmp0;               // unused (BG3's composite before its last effects pass)
layout(set = 0, binding = 4) uniform sampler2D cmp1;               // unused
layout(set = 0, binding = 5) uniform sampler2D cur;                // TAA's input
layout(rgba16f, set = 0, binding = 6) uniform image2D late;  // BG3's share of a pixel it drew over, before its composite and after (wowbg3_blmask.frag, wowbg3_ovl.comp); 0 where the pixel is not WoW's
layout(location = 0) out vec4 o;
layout(push_constant) uniform PC { uint dbg; } pc;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    float n0 = imageLoad(nob, q).r;
    // 1: pure WoW ground at the composite; 0.3: WoW ground with something of BG3's drawn before the composite.
    // Below 0: a decal on WoW's ground (wowbg3_blmask.frag): BG3's TAA stays, and `late` keeps what BG3's effects passes
    // drew over it (wowbg3_ovl.comp) for wdpostcas, as over bare ground.
    if (!(n0 > 0.25)) { if (!(n0 < 0.0)) imageStore(late, q, vec4(0.0)); discard; }
    vec3 c = texelFetch(cur, q, 0).rgb;
    // Pure at the composite (wowbg3_blmask.frag: the forward buffer the composite read was what the feed wrote, the
    // G-buffer bare). This pass used to look at the forward image again here, at the TAA, and call the pixel touched
    // when it differed from the feed; but BG3 uses that image for other things after the composite in some states
    // (turn-based mode, an armed ability with a wide range), so every pixel failed at once and BG3's own relit picture
    // of WoW's ground came back over the whole view (a playtest report: "the fog comes back when I aim, and in
    // turn-based mode").
    // What BG3 drew over WoW's ground is in `late` by now, its share of the pixel a channel: what it drew before its
    // composite (wowbg3_blmask.frag) and, folded in as each of its effects passes ended (wowbg3_ovl.comp, mode 1), what
    // it drew after: the aura of a character aiming, an ability's area on the ground, a spell's wisps and glow, the
    // attack preview's figure. wdpostcas adds that share of BG3's displayed pixel to WoW's own colour, or takes the
    // part a smoke took away.
    vec4 l = imageLoad(late, q);
    if (!(l.a > 0.5)) l = vec4(0.0);
    imageStore(late, q, l);
    // 2: pure and untouched by the bloom's cut; 0.4: WoW's colour with BG3's early addition, which keeps its bloom.
    imageStore(nob, q, vec4(n0 < 0.5 ? 0.4 : ((pc.dbg & 2u) == 0u ? 2.0 : 1.0)));
    o = (pc.dbg & 1u) != 0u ? vec4(0.0, 8.0, 0.0, 1.0) : vec4(c, 1.0);
}
