#version 450
// SDL_GPU: vertex uniform buffers live in set 1.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(set = 1, binding = 0) uniform VSUniforms {
    mat4 viewProj;
    mat4 model;
    vec4 lightDir;      // xyz = direction toward the light
    vec4 lightColor;    // rgb * diffuse
    vec4 ambient;       // rgb * ambient, a = lighting enabled (0/1)
    vec4 eye;           // camera position
};
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vDist;
void main() {
    vec4 wp = model * vec4(inPos, 1.0);
    gl_Position = viewProj * wp;
    vec3 n = normalize(mat3(model) * inNormal);
    float d = max(dot(n, lightDir.xyz), 0.0);  // mission light vector points toward the light
    vec3 lit = mix(vec3(1.0), ambient.rgb + lightColor.rgb * d, ambient.a);
    vColor = vec4(inColor.rgb * lit, inColor.a);
    vUV = inUV;
    vDist = distance(wp.xyz, eye.xyz);
}
