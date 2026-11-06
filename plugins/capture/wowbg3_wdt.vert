// Gate a prototype: WoW's collision ground, moved 0.3 m toward the eye, marks where the ground is the visible surface.
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
layout(location = 1) out float v_z;
void main() {
    vec3 p = a_pos - a_eye.xyz;
    gl_Position = mat4(a_vp0, a_vp1, a_vp2, a_vp3) * vec4(p - a_fwd.xyz * 0.3, 1.0);
    gl_Position.y = -gl_Position.y;
    v_n = vec3(dot(a_nrm, a_right.xyz), dot(a_nrm, a_up.xyz), dot(a_nrm, a_fwd.xyz));
    v_z = dot(p, a_fwd.xyz);
}
