// WOW-BG3 capture layer: WoW's ground drawn into BG3's G-buffer (wowbg3_ground.inc.c), fragment stage.
// Writes what BG3's own level materials write there (their G-buffer shaders, research):
//  0 normal (R16G16): view-space normal n encoded as BG3's passes decode it,
//    s = (x, y) * 3.5554 - 1.7777, z = 1; n = (s.xy * 2 / |s|^2, 1 - 2 / |s|^2);
//  1 albedo (RGBA8): a neutral grey (only BG3's own picture shows it; the compositor never does);
//  2 surface (RGBA8): .w the material byte: shading model 0 (bits 0..2, default lit), bit 3 "receives
//    decals" (BG3's pass 7d71be5d copies the normal into the decal normal only where it is set; its
//    projected decals, EoCSurfaceMeshGenerate and the sight overlay read that normal);
//  3 (the layer's own R32F image, not BG3's): the ground's view depth in metres, 0 where BG3's own
//    geometry stays (cleared): where WoW's ground is BG3's visible surface (wowbg3_surf.inc.c).
// See wowbg3_ground.vert for regenerating the SPIR-V header.
#version 450
layout(location = 0) in vec3 v_n;
layout(location = 1) in float v_z;
layout(location = 0) out vec4 o_normal;
layout(location = 1) out vec4 o_albedo;
layout(location = 2) out vec4 o_surface;
layout(location = 3) out vec4 o_z;
void main() {
    vec3 n = normalize(v_n);
    if (n.z > 0.0) n = -n;  // a face seen from behind (models are two-sided here): toward the eye
    float k = max(1.0 - n.z, 1e-4);
    o_normal = vec4((n.xy / k + 1.7777) / 3.5554, 0.0, 0.0);
    o_albedo = vec4(0.5, 0.5, 0.5, 1.0);
    o_surface = vec4(0.0, 0.5, 1.0, 8.0 / 255.0);
    o_z = vec4(max(v_z, 1e-3), 0.0, 0.0, 0.0);
}
