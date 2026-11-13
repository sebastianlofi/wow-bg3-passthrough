// fx-route prototype: WoW's picture as the background of BG3's effects. Runs just before BG3's effects
// pass on its lit HDR target. Each pixel of BG3's frame N looks up WoW's newest frame K (the camera
// records give both views; WoW's own depth gives the parallax), and WoW's displayed value goes back
// through a table measured from BG3's own output ("fxcal").
#version 450
layout(std430, set = 0, binding = 0) readonly buffer Wow { uint w[]; } wow;
// BG3's exposure: its two buffers as its exposure pass binds them (storage buffers; as uniform buffers they broke
// Nvidia's driver: no uniform usage, storage alignment), at their dynamic offsets.
layout(std430, set = 0, binding = 1) readonly buffer E0 { vec4 a; } e0;
layout(std430, set = 0, binding = 2) readonly buffer E1 { vec4 a; } e1;
layout(push_constant) uniform PC {
    vec4 fN, rN, uN;     // frame N's axes; fN.w tan x, rN.w tan y, uN.w 1/W
    vec4 eK, fK, rK, uK; // K's eye minus N's eye (m), its axes; eK.w tan x, fK.w tan y, rK.w 1/H
    uint ww, wh, cbpr, dbpr, code, flags; // flags 1: decode code 2; 2: ramp (calibration)
    float wnear, wzoff;
    float gamma;
    float jx, jy;  // wdjit (flag 128): BG3's TAA jitter of this frame, NDC y down
} pc;
layout(location = 0) out vec4 o;
layout(r32f, set = 0, binding = 4) uniform readonly image2D wmask;  // >0: the placeholder is what shows here
layout(std430, set = 0, binding = 3) readonly buffer Tab { float TAB[]; } tb;
layout(set = 0, binding = 5) uniform texture2D bdep;   // BG3's linear depth, as its composite reads it (set 1 binding 0)
layout(set = 0, binding = 6) uniform texture3D bfrox;  // its froxel volume (set 1 binding 21)
layout(set = 0, binding = 7) uniform sampler bsamp;
layout(set = 0, binding = 10) uniform texture2D bb1;
layout(set = 0, binding = 11) uniform texture2D bb2;
layout(set = 0, binding = 12) uniform texture2D bb3;    // its sampler (set 1 binding 23)
layout(std140, set = 0, binding = 8) uniform U30 { vec4 v[33]; } u30;  // its set 1 binding 30
layout(std140, set = 0, binding = 9) uniform U4 { uvec4 q[3]; } u4;    // its set 0 binding 4
vec4 frox(float x, float y, float z) {
    vec2 xy = max(u30.v[21].xy, min(u30.v[21].zw, vec2(x, y)));
    return textureLod(sampler3D(bfrox, bsamp), vec3(xy, z), 0.0);
}
// BG3's in-scatter at this pixel: the composite's lookup (54fb164bcf9f7fe8), step for step.
vec4 bg3_fog_st() {
    if (u30.v[25].w < 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    float d = texelFetch(sampler2D(bdep, bsamp), ivec2(gl_FragCoord.xy), 0).r;
    vec2 uv = gl_FragCoord.xy * vec2(pc.uN.w, pc.rK.w);
    float s = d <= u30.v[23].x ? fma(u30.v[23].z, d, u30.v[23].w) : u30.v[24].y * log2(fma(u30.v[24].z, d, u30.v[24].w));
    vec3 c = vec3(uv / vec2(u30.v[32].w, u30.v[22].w), s) + vec3(float(u4.q[2].z), 0.0, 0.0) - vec3(0.5);
    vec3 fl = floor(c), f = c - fl, nf = vec3(1.0) - f;
    vec3 a152 = (nf * 0.166666672) * nf, a154 = (f * 0.5) * f, a159 = (nf * 0.5) * nf, a164 = (f * 0.166666672) * f;
    vec3 w1 = fma(-a154, vec3(2.0) - f, vec3(0.666666687)), w2 = fma(-a159, f + vec3(1.0), vec3(0.666666687)), w3 = a164 * f;
    vec3 g0 = fma(a152, nf, w1), g1 = fma(a164, f, w2);
    vec3 h0 = u30.v[22].xyz * ((w1 / g0 - vec3(0.5)) + fl), h1 = u30.v[22].xyz * ((w3 / g1 + vec3(1.5)) + fl);
    vec4 m203 = mix(frox(h1.x, h0.y, h0.z), frox(h0.x, h0.y, h0.z), g0.x);
    vec4 m222 = mix(frox(h1.x, h1.y, h0.z), frox(h0.x, h1.y, h0.z), g0.x);
    vec4 m225 = mix(m222, m203, g0.y);
    vec4 m238 = mix(frox(h1.x, h0.y, h1.z), frox(h0.x, h0.y, h1.z), g0.x);
    vec4 m250 = mix(frox(h1.x, h1.y, h1.z), frox(h0.x, h1.y, h1.z), g0.x);
    vec4 m251 = mix(m250, m238, g0.y);
    vec4 sc = mix(m251, m225, g0.z) * vec4(0.03125, 0.03125, 0.03125, 3.05175781e-05);
    sc = vec4(isnan(sc.x) || isinf(sc.x) ? 0.0 : sc.x, isnan(sc.y) || isinf(sc.y) ? 0.0 : sc.y,
              isnan(sc.z) || isinf(sc.z) ? 0.0 : sc.z, isnan(sc.w) || isinf(sc.w) ? 0.0 : sc.w);
    return vec4(sc.rgb, 1.0 - sc.w);  // in-scatter, transmittance: the composite writes sum * T + S
}
vec3 bg3_inscatter() { return bg3_fog_st().rgb; }
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
void main() {
    uint dbg = (pc.flags >> 4) & 7u;
    if (dbg >= 3u) { ivec2 q = ivec2(gl_FragCoord.xy); o = vec4((dbg == 3u ? texelFetch(sampler2D(bb2, bsamp), q, 0) : dbg == 4u ? texelFetch(sampler2D(bb3, bsamp), q, 0) : texelFetch(sampler2D(bb1, bsamp), q, 0)).rgb, 1.0); return; }
    if ((pc.flags & 64u) != 0u && dbg == 0u) { o = bg3_fog_st(); return; }  // wdfogst: the S/T image
    float ex = max(e0.a.y * e1.a.x, 1e-6);  // BG3's exposure: the x that shows as w is the table's over this
    vec2 ndc = vec2(gl_FragCoord.x * pc.uN.w, gl_FragCoord.y * pc.rK.w) * 2.0 - 1.0;  // y down
    bool zero = (pc.flags & 4u) != 0u;  // wdfog: the feed only clears; WoW comes in after the composite
    if ((pc.flags & 2u) != 0u) {  // calibration: one colour over the whole frame, 9 steps a channel, changed every 10 presents
        if (zero) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
        int n = int(pc.flags >> 8) % 729;
        ivec3 q = ivec3(n / 81, (n / 9) % 9, n % 9);
        vec3 v = mix(vec3(0.0), exp2(mix(vec3(-9.0), vec3(2.5), (vec3(q) - 1.0) / 7.0)), step(vec3(0.5), vec3(q)));
        o = vec4(v / ex, 1.0);
        return;
    }
    if (!(imageLoad(wmask, ivec2(gl_FragCoord.xy)).r > 0.0)) discard;
    if (zero) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
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
    if ((pc.flags & 32u) != 0u) { o = vec4(bg3_inscatter(), 1.0); return; }
    if ((pc.flags & 16u) != 0u) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
    o = vec4(max(inv3(w) / ex - bg3_inscatter(), vec3(0.0)), 1.0);  // after BG3's composite: W out, BG3's in-scatter in
}
