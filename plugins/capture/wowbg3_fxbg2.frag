// fx-route prototype: WoW's picture as the background of BG3's effects. Runs just before BG3's effects
// pass on its lit HDR target. Each pixel of BG3's frame N looks up WoW's newest frame K (the camera
// records give both views; WoW's own depth gives the parallax), and WoW's displayed value goes back
// through a table measured from BG3's own output ("fxcal").
#version 450
layout(std430, set = 0, binding = 0) readonly buffer Wow { uint w[]; } wow;
layout(std140, set = 0, binding = 1) uniform E0 { vec4 a; } e0;
layout(std140, set = 0, binding = 2) uniform E1 { vec4 a; } e1;
layout(push_constant) uniform PC {
    vec4 fN, rN, uN;     // frame N's axes; fN.w tan x, rN.w tan y, uN.w 1/W
    vec4 eK, fK, rK, uK; // K's eye minus N's eye (m), its axes; eK.w tan x, fK.w tan y, rK.w 1/H
    uint ww, wh, cbpr, dbpr, code, flags; // flags 1: decode code 2; 2: ramp (calibration)
    float wnear, wzoff;
    float gamma;
    float jx, jy;  // wdjit (flag 128): BG3's TAA jitter of this frame, NDC y down
    float evk;     // patch61 (flag 8): 1.2*2^EV_ref, WoW's ground has radiance table(w)/E_ref
    vec4 amb;      // WoW's ambient colour times its share (as WoW displays it, 0..1); a 1: add it on BG3's own pixels
    float fog0, fog1, fogon; uint fogc;  // WoW's fog: start and end (yards of eye depth), 1 when on, the colour 10 bits a channel
} pc;
layout(location = 0) out vec4 o;
layout(r32f, set = 0, binding = 4) uniform readonly image2D wmask;  // >0: the placeholder is what shows here
layout(std430, set = 0, binding = 3) readonly buffer Tab { float TAB[]; } tb;
layout(rgba16f, set = 0, binding = 5) uniform readonly image2D stimg;
layout(rgba32f, set = 0, binding = 6) uniform writeonly image2D wfed;  // patch28: what the feed wrote (wdbloom)  // wdfogst: BG3's S and T at the last composite
layout(set = 0, binding = 9) uniform sampler2D gz;    // BG3's G-buffer eye depth, metres (WoW's fog on BG3's pixels)
layout(set = 0, binding = 8) uniform sampler2D galb;  // BG3's G-buffer albedo (WoW's ambient on BG3's pixels)
layout(rgba16f, set = 0, binding = 7) uniform writeonly image2D wdsp;  // WoW's own displayed colour at this pixel, no jitter (a 1): wdpostcas
float s2l(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3 wow_disp(uint x, uint y) {
    uint v = wow.w[(y * pc.cbpr + x * 4u) >> 2];
    vec3 b = vec3(float(v & 0xffu), float((v >> 8) & 0xffu), float((v >> 16) & 0xffu)) / 255.0;
    vec3 rgb = (pc.code == 3u || pc.code == 4u) ? b.zyx : b;
    if (pc.code == 2u && (pc.flags & 1u) != 0u) {
        rgb = vec3(s2l(rgb.r), s2l(rgb.g), s2l(rgb.b));
        if (pc.gamma != 1.0) rgb = pow(max(rgb, vec3(1e-6)), vec3(pc.gamma));
    }
    return rgb;
}
float wow_z(uint x, uint y) {
    float d = uintBitsToFloat(wow.w[(pc.cbpr * pc.wh + y * pc.dbpr + x * 4u) >> 2]);
    return d > 0.0 ? (pc.wnear / d + pc.wzoff) * 0.9144 : 1e30;
}
vec3 tab(int r, int g, int b) { int i = (r * 289 + g * 17 + b) * 3; return vec3(tb.TAB[i], tb.TAB[i + 1], tb.TAB[i + 2]); }
// log2-free trilinear: the table holds log2(x) so dark colours stay accurate.
vec3 inv3(vec3 w) {
    vec3 p = clamp(w, vec3(0.0), vec3(1.0)) * 16.0;
    ivec3 i = min(ivec3(p), ivec3(15)); vec3 f = p - vec3(i);
    vec3 c00 = mix(tab(i.x, i.y, i.z), tab(i.x + 1, i.y, i.z), f.x), c10 = mix(tab(i.x, i.y + 1, i.z), tab(i.x + 1, i.y + 1, i.z), f.x);
    vec3 c01 = mix(tab(i.x, i.y, i.z + 1), tab(i.x + 1, i.y, i.z + 1), f.x), c11 = mix(tab(i.x, i.y + 1, i.z + 1), tab(i.x + 1, i.y + 1, i.z + 1), f.x);
    return exp2(mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z));
}
// WoW's displayed colour where frame N's ray through `ndc` lands in WoW's frame K.
vec3 wow_at(vec2 ndc) {
    vec3 dir = pc.fN.xyz + pc.rN.xyz * (ndc.x * pc.fN.w) - pc.uN.xyz * (ndc.y * pc.rN.w);
    uint qx = min(uint(gl_FragCoord.x * float(pc.ww) * pc.uN.w), pc.ww - 1u), qy = min(uint(gl_FragCoord.y * float(pc.wh) * pc.rK.w), pc.wh - 1u);
    vec2 t = vec2(float(qx), float(qy));
    for (int k = 0; k < 2; k++) {
        float z = wow_z(qx, qy);
        vec3 v = z < 1e20 ? dir * z - pc.eK.xyz : dir;
        float zk = dot(v, pc.fK.xyz);
        if (!(zk > 0.02)) break;
        vec2 n = vec2(dot(v, pc.rK.xyz) / (zk * pc.eK.w), dot(v, pc.uK.xyz) / (zk * pc.fK.w));
        t = vec2((n.x + 1.0) * 0.5 * float(pc.ww), (1.0 - n.y) * 0.5 * float(pc.wh)) - 0.5;
        t = clamp(t, vec2(0.0), vec2(float(pc.ww) - 1.0, float(pc.wh) - 1.0));
        qx = uint(t.x + 0.5); qy = uint(t.y + 0.5);
    }
    vec2 fl = floor(t), f = t - fl;
    uint x0 = uint(fl.x), y0 = uint(fl.y), x1 = min(x0 + 1u, pc.ww - 1u), y1 = min(y0 + 1u, pc.wh - 1u);
    vec3 w = mix(mix(wow_disp(x0, y0), wow_disp(x1, y0), f.x), mix(wow_disp(x0, y1), wow_disp(x1, y1), f.x), f.y);
    return w;
}
void main() {
    float ex = max(e0.a.y * e1.a.x, 1e-6);  // BG3's exposure: the x that shows as w is the table's over this
    // patch61: e0 is BG3's newest eye state (the exposure this frame's lighting pre-exposes with): WoW's radiance
    // table(w)/E_ref pre-exposed as BG3's lighting writes its own ground.
    if ((pc.flags & 8u) != 0u && e0.a.x > 0.0) ex = 1.0 / (e0.a.x * pc.evk);
    vec2 ndc = vec2(gl_FragCoord.x * pc.uN.w, gl_FragCoord.y * pc.rK.w) * 2.0 - 1.0;  // y down
    if ((pc.flags & 128u) != 0u) ndc -= vec2(pc.jx, pc.jy);  // wdjit: the ray BG3's jittered projection gives this pixel
    bool zero = (pc.flags & 4u) != 0u;  // wdfog: the feed only clears; WoW comes in after the composite
    if ((pc.flags & 2u) != 0u) {  // calibration: one colour over the whole frame, 9 steps a channel, changed every 10 presents
        if (zero) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
        int n = int(pc.flags >> 8) % 729;
        ivec3 q = ivec3(n / 81, (n / 9) % 9, n % 9);
        vec3 v = mix(vec3(0.0), exp2(mix(vec3(-9.0), vec3(2.5), (vec3(q) - 1.0) / 7.0)), step(vec3(0.5), vec3(q)));
        o = vec4(v / ex, 1.0);
        imageStore(wfed, ivec2(gl_FragCoord.xy), o);
        return;
    }
    if (!(imageLoad(wmask, ivec2(gl_FragCoord.xy)).r > 0.0)) {
        // BG3's own pixel: WoW's ambient term for it, added to what BG3 lit (alpha 0: the blend keeps BG3's light).
        // WoW shows a unit's texture times (ambient + sun); the ambient part alone would show as texture x ambient,
        // and the table gives the light that shows as that colour. Black adds nothing (the table's own floor is
        // taken off), and so does the placeholder, whose albedo is black.
        // WoW's fog for the pixel, for wdpostcas: its factor by the pixel's eye depth (1: none), beside the fog's colour.
        // Every one of BG3's pixels is written, so none keeps what an earlier frame left there.
        ivec2 q = ivec2(gl_FragCoord.xy);
        vec4 fg = vec4(0.0);
        if (pc.fogon > 0.5 && !zero) {
            float zy = texelFetch(gz, q, 0).r / 0.9144;
            float f = clamp((pc.fog1 - zy) / max(pc.fog1 - pc.fog0, 0.001), 0.0, 1.0);
            if (zy < 1e6) fg = vec4(float(pc.fogc & 1023u), float((pc.fogc >> 10) & 1023u), float((pc.fogc >> 20) & 1023u), 0.0) / 1023.0 + vec4(0.0, 0.0, 0.0, 2.0 + f);
        }
        imageStore(wdsp, q, fg);
        o = vec4(0.0);
        if (!(pc.amb.a > 0.5) || zero) return;
        vec3 al = texelFetch(galb, q, 0).rgb;
        if (max(al.r, max(al.g, al.b)) < 0.5 / 255.0) return;
        vec3 tex = pow(clamp(al, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
        o = vec4(max(inv3(tex * pc.amb.rgb) - inv3(vec3(0.0)), vec3(0.0)) / ex, 0.0);
        return;
    }
    if (zero) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 w = wow_at(ndc);
    // BG3's display transform cannot show every colour WoW draws (a bright saturated one has no value that comes out
    // as itself), so the pixel's own colour is kept beside what is fed, for after that transform.
    imageStore(wdsp, ivec2(gl_FragCoord.xy), vec4((pc.flags & 128u) != 0u ? wow_at(ndc + vec2(pc.jx, pc.jy)) : w, 1.0));
    vec3 W = inv3(w) / ex;
    if ((pc.flags & 64u) != 0u) { vec4 st = imageLoad(stimg, ivec2(gl_FragCoord.xy)); W = max((W - st.rgb) / max(st.a, 1e-4), vec3(0.0)); }
    o = vec4(W, 1.0);
    imageStore(wfed, ivec2(gl_FragCoord.xy), o);
}
