// WOW-BG3 capture layer (wowbg3_shade.inc.c): a disc facing the light, between a shaded figure and the sun, drawn into
// one of BG3's sun shadow maps with that map's own view (camera-relative view-projection and eye, as BG3's own
// shadow draws use them; y flipped as BG3's vertex shaders flip it for Vulkan, see wowbg3_ground.vert).
// 16 triangles round the centre: vertex 0 of each is the centre, 1 and 2 the rim.
#version 450
layout(push_constant) uniform PC { mat4 vp; vec4 eye_r; vec4 centre; vec4 fwd; } pc;
void main() {
    int tri = gl_VertexIndex / 3, k = gl_VertexIndex % 3;
    vec3 f = normalize(pc.fwd.xyz);
    vec3 a = normalize(cross(abs(f.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), f));
    vec3 b = cross(f, a);
    float ang = 6.28318530718 * float(tri + (k == 2 ? 1 : 0)) / 16.0;
    vec3 p = pc.centre.xyz + (k == 0 ? vec3(0.0) : (a * cos(ang) + b * sin(ang)) * pc.eye_r.w);
    gl_Position = pc.vp * vec4(p - pc.eye_r.xyz, 1.0);
    gl_Position.y = -gl_Position.y;
}
