#version 450
// SDL_GPU: fragment samplers in set 2, fragment uniform buffers in set 3.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vDist;
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform FSUniforms {
    vec4 fogColor;   // rgb, a = fog intensity (max blend)
    vec4 fogRange;   // x = start, y = end, z = alpha test ref, w = alpha mul
};
void main() {
    vec4 c = texture(tex, vUV) * vColor;
    if (c.a < fogRange.z) discard;
    c.a *= fogRange.w;
    float f = clamp((vDist - fogRange.x) / max(fogRange.y - fogRange.x, 0.001), 0.0, 1.0) * fogColor.a;
    outColor = vec4(mix(c.rgb, fogColor.rgb, f), c.a);
}
