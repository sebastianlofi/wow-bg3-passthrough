// WOW-BG3 capture layer: the ground mark of the wdepth path (wd_mark in wowbg3_wd.inc.c), fragment stage.
// WoW's collision ground is drawn over BG3's G-buffer after WoW's depth has been written into it; where a triangle
// is the surface WoW shows, the pixel gets the ground's own normal and the "receives decals" byte (BG3's decals,
// surfaces and sight cones land on WoW's ground and not on its models and leaves).
//
// "Is the surface WoW shows" was the depth test alone: the triangle, 0.3 m toward the eye, at or nearer than the
// depth there. That also passes for a triangle far in FRONT of what is there, and WoW's collision has triangles its
// picture does not: a wall or a ceiling the see-through opening has cut away, a storey taken off over the hero.
// Such a triangle between the camera and a character overwrote the character's normal and material in BG3's
// G-buffer: one wall's normal over the whole figure, no light of BG3's on it (a playtest report: the character
// loses its lighting and detail at some camera angles; seen in a ring of angles with wdzdbg=1).
// So the triangle must also lie where WoW's own depth for the pixel is (the same reading as wowbg3_wd.frag),
// within TOL metres plus TOL_REL of the distance. The depth test stays, for BG3's own geometry in front.
// Regenerate wowbg3_wdm_spv.h after editing (glslang-tools, e.g. in the Steam runtime's SDK image):
//   glslangValidator -V --target-env vulkan1.0 --vn wowbg3_wdm_frag -o wowbg3_wdm_frag.h wowbg3_wdm.frag
//   sed -e 's/^const uint32_t/static const uint32_t/' -e '/#pragma once/d' wowbg3_wdm_frag.h > wowbg3_wdm_spv.h
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
layout(location = 0) in vec3 v_n;
layout(location = 1) in float v_z;
layout(location = 0) out vec4 o_normal;
layout(location = 1) out vec4 o_albedo;
layout(location = 2) out vec4 o_surface;
layout(location = 3) out vec4 o_z;
const float TOL = 0.3;       // metres: WoW's collision against what it draws, and the depth's own steps
const float TOL_REL = 0.02;  // of the distance
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
    if (!(z < 1e20)) discard;                              // WoW shows its sky here: no ground
    if (abs(v_z - z) > TOL + TOL_REL * z) discard;         // not the surface WoW shows at this pixel
    vec3 n = normalize(v_n);
    if (n.z > 0.0) n = -n;  // a face seen from behind (models are two-sided here): toward the eye
    float k = max(1.0 - n.z, 1e-4);
    o_normal = vec4((n.xy / k + 1.7777) / 3.5554, 0.0, 0.0);
    o_albedo = vec4(0.0, 0.0, 0.0, 1.0);                   // not written (the mark's pipeline masks it)
    o_surface = vec4(1.0, 0.0, 0.0, 8.0 / 255.0);
    o_z = vec4(max(v_z, 1e-3), 0.0, 0.0, 0.0);             // not written
}
