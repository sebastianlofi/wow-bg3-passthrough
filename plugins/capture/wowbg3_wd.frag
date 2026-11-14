// Gate a prototype: WoW's depth per pixel as BG3's scene depth, with a neutral placeholder G-buffer.
// Runs in the ground injection's render pass, right after BG3's G-buffer pass. Word "wdepth".
#version 450
layout(std430, set = 0, binding = 0) readonly buffer Wow { uint w[]; } wow;
layout(push_constant) uniform PC {
    mat4 inv;        // inverse of BG3's camera-relative view-projection, frame N
    vec4 fwdN;       // frame N's axes
    vec4 rightN;
    vec4 upN;
    vec4 rowZ;       // the view-projection's z and w rows
    vec4 rowW;
    vec4 eyeK;       // WoW frame K's eye minus N's eye; w tan x
    vec4 fwdK;       // w tan y
    vec4 rightK;
    vec4 upK;
    uint ww, wh, cbpr, dbpr;
    float wnear, wzoff, invW, invH;
    float occ;       // BG3's occlusion for the placeholder (albedo alpha; word wdocc)
} pc;
layout(location = 0) out vec4 o_normal;
layout(location = 1) out vec4 o_albedo;
layout(location = 2) out vec4 o_surface;
layout(location = 3) out vec4 o_z;
float wow_z(uint x, uint y) {
    float d = uintBitsToFloat(wow.w[(pc.cbpr * pc.wh + y * pc.dbpr + x * 4u) >> 2]);
    return d > 0.0 ? (pc.wnear / d + pc.wzoff) * 0.9144 : 1e30;
}
void main() {
    vec2 ndc = gl_FragCoord.xy * vec2(pc.invW, pc.invH) * 2.0 - 1.0;
    vec4 h1 = pc.inv * vec4(ndc.x, -ndc.y, 0.5, 1.0);
    vec4 h2 = pc.inv * vec4(ndc.x, -ndc.y, 0.02, 1.0);
    vec3 p1 = h1.xyz / h1.w, dd = h2.xyz / h2.w - p1;
    float den = dot(dd, pc.fwdN.xyz);
    if (!(abs(den) > 1e-9)) discard;
    uint qx = min(uint(gl_FragCoord.x * pc.invW * float(pc.ww)), pc.ww - 1u), qy = min(uint(gl_FragCoord.y * pc.invH * float(pc.wh)), pc.wh - 1u);
    float z = wow_z(qx, qy);
    // WoW's frame K may have an older camera: follow the point into it, twice.
    for (int k = 0; k < 2; k++) {
        vec3 r = z < 1e20 ? p1 + dd * ((z - dot(p1, pc.fwdN.xyz)) / den) : dd * 1e4 / sqrt(dot(dd, dd));
        vec3 v = r - pc.eyeK.xyz;
        float zk = dot(v, pc.fwdK.xyz);
        if (!(zk > 0.02)) break;
        vec2 n = vec2(dot(v, pc.rightK.xyz) / (zk * pc.eyeK.w), dot(v, pc.upK.xyz) / (zk * pc.fwdK.w));
        vec2 t = clamp(vec2((n.x + 1.0) * 0.5 * float(pc.ww), (1.0 - n.y) * 0.5 * float(pc.wh)) - 0.5, vec2(0.0), vec2(float(pc.ww) - 1.0, float(pc.wh) - 1.0));
        qx = uint(t.x + 0.5); qy = uint(t.y + 0.5);
        z = wow_z(qx, qy);
    }
    if (!(z < 1e20)) {  // WoW's sky: placeholder at the far end, so whatever BG3 drew stays in front
        gl_FragDepth = 1e-7;
        o_normal = vec4(0.5, 0.5, 0.0, 0.0);
        o_albedo = vec4(vec3(46.0 / 255.0), pc.occ);  // the bare placeholder (wowbg3_ground.frag)
        o_surface = vec4(1.0, 0.0, 0.0, 0.0);
        o_z = vec4(1e7, 0.0, 0.0, 0.0);
        return;
    }
    vec3 p = p1 + dd * ((z - dot(p1, pc.fwdN.xyz)) / den);
    float w = dot(pc.rowW, vec4(p, 1.0));
    if (!(w > 1e-4)) discard;
    gl_FragDepth = clamp(dot(pc.rowZ, vec4(p, 1.0)) / w, 0.0, 1.0);
    vec3 vs = vec3(dot(p, pc.rightN.xyz), dot(p, pc.upN.xyz), dot(p, pc.fwdN.xyz));
    // Facing from the nearer of each side's neighbours, so a depth step gives no wild normal; none finite: toward the eye.
    vec3 cr = cross(dFdxFine(vs), dFdyFine(vs));
    float l2 = dot(cr, cr);
    vec3 n = (l2 > 1e-14 && l2 < 1e14) ? cr / sqrt(l2) : vec3(0.0, 0.0, -1.0);
    if (n.z > 0.0) n = -n;
    if (any(isnan(n)) || any(isinf(n))) n = vec3(0.0, 0.0, -1.0);
    float k = max(1.0 - n.z, 1e-4);
    o_normal = vec4((n.xy / k + 1.7777) / 3.5554, 0.0, 0.0);
    o_albedo = vec4(vec3(46.0 / 255.0), pc.occ);  // the bare placeholder (wowbg3_ground.frag)
    o_surface = vec4(1.0, 0.0, 0.0, 0.0);  // no "receives decals": the ground mark sets it
    o_z = vec4(max(z, 1e-3), 0.0, 0.0, 0.0);
}
