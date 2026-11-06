// fx-route prototype (wdbare, wowbg3_fxbg.inc.c): 0 into BG3's lighting buffer at WoW's pixels whose G-buffer is
// still the bare placeholder (albedo 0, surface 1 0 0 with shading model 0); everything else is left as BG3 lit it.
#version 450
layout(r32f, set = 0, binding = 0) uniform readonly image2D wmask;  // >0: the placeholder is what shows here
layout(set = 0, binding = 1) uniform sampler2D alb;   // BG3's G-buffer albedo (alpha its occlusion)
layout(set = 0, binding = 2) uniform sampler2D surf;  // roughness, specular level, metalness, material byte
layout(location = 0) out vec4 o;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (!(imageLoad(wmask, q).r > 0.0)) discard;
    vec4 a = texelFetch(alb, q, 0), s = texelFetch(surf, q, 0);
    uint mat = uint(s.w * 255.0 + 0.5);
    bool bare = all(lessThan(a.rgb, vec3(0.5 / 255.0))) && s.x > 254.5 / 255.0 && s.y < 0.5 / 255.0 && s.z < 0.5 / 255.0 && (mat & 0xf7u) == 0u;
    if (!bare) discard;
    o = vec4(0.0, 0.0, 0.0, 1.0);
}
