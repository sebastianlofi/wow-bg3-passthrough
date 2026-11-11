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
layout(set = 0, binding = 10) uniform sampler2D gn;   // BG3's G-buffer normal (the facing WoW's light falls on)
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
const float ROOM_FLOOR = 0.30;  // the least light, as WoW counts it (0..1), on a BG3 character in a room
// WoW's ambient is one colour whatever a face's direction, and WoW's textures carry their own painted shading under
// it. BG3's albedo carries none (its form comes from light on its normals), so under a flat ambient a BG3 figure
// shows as flat colour. The ambient falls by the facing instead: this much more on a face turned up, as much less
// on one turned down, the same on one turned sideways.
const float AMB_SHAPE = 0.35;
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
// A pixel's place in frame N's view from its eye depth: right, up, forward (metres).
vec3 vpos(vec2 ndc, float z) { return vec3(ndc.x * pc.fN.w * z, -ndc.y * pc.rN.w * z, z); }
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
        const int LT = 17 * 17 * 17 * 3;
        const int LX = LT + 4 + 8 * 8 + 8 * 8;  // the tail's last four: the world's up in this view, 1 with BG3's normal bound
        int nl = int(tb.TAB[LT]), nb = int(tb.TAB[LT + 2]);
        // The pixel's place in this view (right, up, forward: metres) and its facing. BG3's own normal for it when
        // its lighting pass has bound one (its materials' detail is in it: BG3 decodes s = xy * 3.5554 - 1.7777,
        // n = (s * 2 / (|s|^2 + 1), 1 - 2 / (|s|^2 + 1))); else from the eye depths beside it: on each axis the
        // neighbour nearer in depth, so the step at a figure's edge gives no wild facing.
        float z = texelFetch(gz, q, 0).r;
        vec3 P = vpos(ndc, z);
        vec3 N;
        if (tb.TAB[LX + 3] > 0.5) {
            vec2 e = texelFetch(gn, q, 0).rg * 3.5554 - 1.7777;
            float k = 2.0 / (dot(e, e) + 1.0);
            N = vec3(e * k, 1.0 - k);
        } else {
            vec2 px = vec2(pc.uN.w, pc.rK.w) * 2.0;
            float zl = texelFetch(gz, q + ivec2(-1, 0), 0).r, zr = texelFetch(gz, q + ivec2(1, 0), 0).r;
            float zu = texelFetch(gz, q + ivec2(0, -1), 0).r, zd = texelFetch(gz, q + ivec2(0, 1), 0).r;
            vec3 dx = abs(zr - z) < abs(zl - z) ? vpos(ndc + vec2(px.x, 0.0), zr) - P : P - vpos(ndc - vec2(px.x, 0.0), zl);
            vec3 dy = abs(zd - z) < abs(zu - z) ? vpos(ndc + vec2(0.0, px.y), zd) - P : P - vpos(ndc - vec2(0.0, px.y), zu);
            N = cross(dx, dy);
            float nn = dot(N, N);
            N = nn > 1e-20 ? N / sqrt(nn) : vec3(0.0, 0.0, -1.0);
            if (dot(N, P) > 0.0) N = -N;
        }
        float shape = 1.0 + AMB_SHAPE * dot(N, vec3(tb.TAB[LX], tb.TAB[LX + 1], tb.TAB[LX + 2]));
        // WoW's light sum for a unit: the ambient, and its three nearest point lights within their range (after the
        // table in its buffer: count, share, then place in this view, range, colour a light).
        vec3 lit = pc.amb.rgb * shape;
        if (nl > 0 || nb > 0) {
            vec3 sum = vec3(0.0);
            if (nb > 0) {
                // Lobes, each its colour by the facing (after the point lights in the buffer: toward the light in
                // this view, colour). In a room WoW lights a unit by the room alone: the floor's baked light on its
                // fixed axis and the room's lights, and the outdoor law's point lights do not apply (the header's
                // fourth number says so). Outdoors the one lobe is the sun's share WoW leaves a unit in the shade.
                const int LB = LT + 4 + 8 * 8;
                for (int i = 0; i < nb && i < 8; i++) {
                    int b = LB + i * 8;
                    // WoW shades a unit through an order-2 SH probe of these lobes, not a hard N.L: a face turned from
                    // the light keeps a little of it (0.094 at a right angle, 0.0625 straight away), which in a dim
                    // room is as much as the ambient.
                    float c = dot(N, vec3(tb.TAB[b], tb.TAB[b + 1], tb.TAB[b + 2]));
                    sum += vec3(tb.TAB[b + 4], tb.TAB[b + 5], tb.TAB[b + 6]) * max(0.25 + 0.5 * c + 0.3125 * (1.5 * c * c - 0.5), 0.0);
                }
            }
            if (!(tb.TAB[LT + 3] > 0.5)) {
                float sd[3] = float[3](1e30, 1e30, 1e30); int sel[3] = int[3](0, 0, 0);
                for (int i = 0; i < nl && i < 8; i++) {
                    int b = LT + 4 + i * 8;
                    vec3 dv = (vec3(tb.TAB[b], tb.TAB[b + 1], tb.TAB[b + 2]) - P) / 0.9144;
                    float d2 = dot(dv, dv), rg = tb.TAB[b + 3];
                    if (d2 > rg * rg) continue;
                    if (d2 < sd[0]) { sd[2] = sd[1]; sel[2] = sel[1]; sd[1] = sd[0]; sel[1] = sel[0]; sd[0] = d2; sel[0] = b; }
                    else if (d2 < sd[1]) { sd[2] = sd[1]; sel[2] = sel[1]; sd[1] = d2; sel[1] = b; }
                    else if (d2 < sd[2]) { sd[2] = d2; sel[2] = b; }
                }
                for (int s = 0; s < 3; s++) {
                    if (sd[s] > 9.9e29) break;
                    int b = sel[s];
                    vec3 tl = (vec3(tb.TAB[b], tb.TAB[b + 1], tb.TAB[b + 2]) - P) / 0.9144;
                    float d = length(tl);
                    sum += vec3(tb.TAB[b + 4], tb.TAB[b + 5], tb.TAB[b + 6]) * (max(dot(N, tl / max(d, 1e-4)), 0.0) / (0.7 * d + 0.03 * d * d));
                }
            }
            lit += sum * tb.TAB[LT + 1];
        }
        // In a room, never less than a small neutral light on BG3's own pixels (its characters and what they carry;
        // the ground, surfaces and everything of WoW's are not these pixels). WoW's rule alone leaves a figure in a
        // dark room near black: its units stay readable there by their bright painted textures, BG3's materials are
        // far darker and take a dim blue room's light as almost nothing (the maintainer's call: a readable floor, on the
        // characters alone).
        if (tb.TAB[LT + 3] > 0.5) lit = max(lit, vec3(ROOM_FLOOR * shape));
        o = vec4(max(inv3(tex * min(lit, vec3(1.0))) - inv3(vec3(0.0)), vec3(0.0)) / ex, 0.0);
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
    // What BG3 laid on the ground here (a surface, a decal: its albedo over the placeholder's black) is lit by BG3's
    // sun alone, as its characters were: black wherever that sun does not reach, in a shadow or behind the blocker
    // that keeps the sun off a figure in WoW's shade. WoW's ambient for it, as for BG3's own pixels (after wfed: the
    // mask compares the forward buffer with what stands there for WoW alone).
    if (pc.amb.a > 0.5) {
        vec3 al = texelFetch(galb, ivec2(gl_FragCoord.xy), 0).rgb;
        if (max(al.r, max(al.g, al.b)) >= 0.5 / 255.0) {
            vec3 tex = pow(clamp(al, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
            o.rgb += max(inv3(tex * min(pc.amb.rgb, vec3(1.0))) - inv3(vec3(0.0)), vec3(0.0)) / ex;
        }
    }
}
