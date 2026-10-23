// WOW-BG3 capture layer: WoW's colours in BG3's G-buffer albedo (wowbg3_surf.inc.c), vertex stage:
// one triangle over the whole target.
// Regenerate wowbg3_surf_spv.h after editing (glslang-tools):
//   glslangValidator -V --target-env vulkan1.0 --vn wowbg3_surf_alb_vert -o a.h wowbg3_surf_alb.vert
//   glslangValidator -V --target-env vulkan1.0 --vn wowbg3_surf_alb_frag -o b.h wowbg3_surf_alb.frag
//   cat a.h b.h > wowbg3_surf_spv.h
#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
