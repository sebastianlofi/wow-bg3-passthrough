// WOW-BG3 capture layer: WoW's colours in BG3's G-buffer albedo (wowbg3_surf.inc.c), fragment stage.
// Runs at the end of BG3's command buffer that holds its G-buffer pass, after the layer drew WoW's
// ground into it (wowbg3_ground.inc.c). Where WoW's ground is BG3's visible surface (the ground's
// view depth image, Z > 0) the albedo becomes WoW's own colour of that ground point: the point (BG3
// frame N's camera, from Z) projected into a WoW frame K that is already drawn (2 to 4 BG3 frames
// older; its camera is the plugin's record it was drawn from), WoW's colour there, bilinear, in
// linear light (BG3's albedo bytes are linear UNORM). BG3's own surface decals then change that
// colour as they change BG3's ground. Output 1 (the layer's A0 image) keeps the same bytes, so the
// compositor knows exactly what each decal changed; alpha 1 = WoW's frame saw that point, 0.5 =
// WoW's frame shows something nearer there (Tav, a tree: the colour may be that thing's), 0 = not
// WoW's ground.
// Flag 2: WoW's colour divided by WoW's light on level sunlit ground (benilla 0052,
// gamma bytes in the last push constant), so BG3's albedo is the ground's own colour without WoW's
// light, as BG3's own albedo is; the compositor lights each decal's change with the same light.
#version 450
layout(set = 0, binding = 0, r32f) uniform readonly image2D zimg;   // ground view depth (frame N), 0: none
layout(std430, set = 0, binding = 1) readonly buffer Wow { uint w[]; } wow;  // WBF1 payload: colour rows, depth rows
layout(push_constant) uniform PC {
    mat4 inv;       // inverse of BG3's camera-relative view-projection of frame N (clip -> eye-relative world)
    vec4 fwdN;      // frame N's forward axis (xyz)
    vec4 eyeK;      // WoW frame K's eye minus frame N's eye (BG3 metres); w: tan(half horizontal fov)
    vec4 fwdK;      // K's axes in BG3's world; fwdK.w: tan(half vertical fov)
    vec4 rightK;
    vec4 upK;
    uint ww, wh, cbpr, dbpr, code, flags;  // flags: 1 decode code 2 once
    float wnear, wzoff;
    float invW, invH;
    float wgamma, glight;  // glight: the light's bits (RGB gamma bytes, byte 3 = 1)
} pc;
layout(location = 0) out vec4 o_albedo;  // BG3's albedo (RGB written)
layout(location = 1) out vec4 o_a0;      // the layer's A0 image

float s2l(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3 wow_lin(uint x, uint y) {
    if (pc.code == 5u) {  // RGBA16F, linear
        uint a = (y * pc.cbpr + x * 8u) >> 2;
        vec2 rg = unpackHalf2x16(wow.w[a]);
        vec2 ba = unpackHalf2x16(wow.w[a + 1u]);
        return clamp(vec3(rg.x, rg.y, ba.x), 0.0, 1.0);
    }
    uint v = wow.w[(y * pc.cbpr + x * 4u) >> 2];
    vec3 b = vec3(float(v & 0xffu), float((v >> 8) & 0xffu), float((v >> 16) & 0xffu)) / 255.0;
    vec3 rgb = (pc.code == 3u || pc.code == 4u) ? b.zyx : b;  // BGRA bytes: swap to RGB
    if (pc.code == 2u && (pc.flags & 1u) != 0u) {  // the byte benilla shows (wowbg3_compose.comp)
        rgb = vec3(s2l(rgb.r), s2l(rgb.g), s2l(rgb.b));
        if (pc.wgamma != 1.0) rgb = pow(max(rgb, vec3(1e-6)), vec3(pc.wgamma));
    }
    return vec3(s2l(rgb.r), s2l(rgb.g), s2l(rgb.b));
}
float wow_z(uint x, uint y) {  // WoW's view depth there (BG3 metres), 1e30 for none
    float d = uintBitsToFloat(wow.w[(pc.cbpr * pc.wh + y * pc.dbpr + x * 4u) >> 2]);
    return d > 0.0 ? (pc.wnear / d + pc.wzoff) * 0.9144 : 1e30;
}
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    float z = imageLoad(zimg, q).r;
    if (!(z > 0.0)) discard;
    // The ground point: frame N's pixel ray (BG3 flips clip y in its vertex shaders) at depth z.
    // Two points of the pixel's ray (reverse-Z: 0.5 near the eye, 0.02 farther), relative to the
    // origin BG3's camera-relative matrices use, then the point whose depth along forward is z.
    vec2 ndc = gl_FragCoord.xy * vec2(pc.invW, pc.invH) * 2.0 - 1.0;
    vec4 h1 = pc.inv * vec4(ndc.x, -ndc.y, 0.5, 1.0);
    vec4 h2 = pc.inv * vec4(ndc.x, -ndc.y, 0.02, 1.0);
    vec3 p1 = h1.xyz / h1.w, dd = h2.xyz / h2.w - p1;
    float den = dot(dd, pc.fwdN.xyz);
    if (!(abs(den) > 1e-9)) discard;
    vec3 v = p1 + dd * ((z - dot(p1, pc.fwdN.xyz)) / den) - pc.eyeK.xyz;  // the point relative to K's eye
    // ... in WoW frame K
    float zk = dot(v, pc.fwdK.xyz);
    float a = 1.0;
    vec2 t = vec2(0.0);
    if (zk > 0.05) {
        vec2 n = vec2(dot(v, pc.rightK.xyz) / (zk * pc.eyeK.w), dot(v, pc.upK.xyz) / (zk * pc.fwdK.w));
        t = vec2((n.x + 1.0) * 0.5 * float(pc.ww), (1.0 - n.y) * 0.5 * float(pc.wh)) - 0.5;
        if (t.x < -0.5 || t.y < -0.5 || t.x > float(pc.ww) - 0.5 || t.y > float(pc.wh) - 0.5) a = 0.5;  // off WoW's frame: its edge
    } else {
        a = 0.5;
        t = vec2(float(pc.ww), float(pc.wh)) * 0.5;
    }
    t = clamp(t, vec2(0.0), vec2(float(pc.ww) - 1.0, float(pc.wh) - 1.0));
    vec2 fl = floor(t);
    vec2 f = t - fl;
    uint x0 = uint(fl.x), y0 = uint(fl.y), x1 = min(x0 + 1u, pc.ww - 1u), y1 = min(y0 + 1u, pc.wh - 1u);
    vec3 c = mix(mix(wow_lin(x0, y0), wow_lin(x1, y0), f.x), mix(wow_lin(x0, y1), wow_lin(x1, y1), f.x), f.y);
    // WoW's frame has something nearer than this ground point there (Tav, a tree, a fence).
    if (a == 1.0 && wow_z(uint(t.x + 0.5), uint(t.y + 0.5)) < zk * 0.97 - 0.15) a = 0.5;
    if ((pc.flags & 2u) != 0u) {
        uint g = floatBitsToUint(pc.glight);
        vec3 lg = vec3(float(g & 0xffu), float((g >> 8) & 0xffu), float((g >> 16) & 0xffu)) / 255.0;
        c = min(c / max(vec3(s2l(lg.r), s2l(lg.g), s2l(lg.b)), vec3(0.02)), vec3(1.0));
    }
    // Bytes exactly as the UNORM target stores them, so a decal's change is exact.
    c = floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    o_albedo = vec4(c, 1.0);
    o_a0 = vec4(c, a);
}
