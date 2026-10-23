// WOW-BG3 capture layer: WoW's ground drawn into BG3's G-buffer (wowbg3_ground.inc.c), vertex stage.
// Per vertex: a WoW ground triangle's corner in BG3 world metres and its face normal. Per instance
// (one record per frame, written by the layer): BG3's camera-relative view-projection of this frame
// (set 0 binding 2 floats 32..47 as they are: row-vector convention, clip = [p - eye, 1] * M, so the
// four rows are this mat4's columns), its eye (floats 52..54) and the camera's right, up and forward
// axes (the rotation of floats 0..15), so the depth is exactly what BG3 itself would write there.
// Regenerate wowbg3_ground_spv.h after editing (glslang-tools):
//   glslangValidator -V --target-env vulkan1.0 --vn wowbg3_ground_vert -o wowbg3_ground_vert.h wowbg3_ground.vert
//   glslangValidator -V --target-env vulkan1.0 --vn wowbg3_ground_frag -o wowbg3_ground_frag.h wowbg3_ground.frag
//   cat wowbg3_ground_vert.h wowbg3_ground_frag.h > wowbg3_ground_spv.h
#version 450
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_vp0;
layout(location = 3) in vec4 a_vp1;
layout(location = 4) in vec4 a_vp2;
layout(location = 5) in vec4 a_vp3;
layout(location = 6) in vec4 a_eye;
layout(location = 7) in vec4 a_right;
layout(location = 8) in vec4 a_up;
layout(location = 9) in vec4 a_fwd;
layout(location = 0) out vec3 v_n;
layout(location = 1) out float v_z;  // view depth along the camera's forward axis (metres), for the surfaces (wowbg3_surf.inc.c)
void main() {
    gl_Position = mat4(a_vp0, a_vp1, a_vp2, a_vp3) * vec4(a_pos - a_eye.xyz, 1.0);
    // BG3's clip space is y up (its HLSL); its vertex shaders flip it for Vulkan (AiGrid.0:
    // "_300.y = -_300.y"), so the same here or the ground lands mirrored about the screen's middle.
    gl_Position.y = -gl_Position.y;
    // BG3's G-buffer normal is view space in the camera's own axes: (n.right, n.up, n.forward).
    v_n = vec3(dot(a_nrm, a_right.xyz), dot(a_nrm, a_up.xyz), dot(a_nrm, a_fwd.xyz));
    v_z = dot(a_pos - a_eye.xyz, a_fwd.xyz);
}
