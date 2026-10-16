#version 450
// Fog over "nothing known": two octaves of value noise (64px, 160px) mixed 70/30, drifting tint, soft edges.
layout(location = 0) in vec2 vPos;
layout(location = 1) flat in vec4 vRect;
layout(location = 2) flat in vec3 vParams;
layout(push_constant) uniform PC { vec2 viewport; float time; float octave1; vec2 drift; vec2 pad1; vec4 tint; } pc;
layout(location = 0) out vec4 outColor;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + vec2(1, 0)), c = hash(i + vec2(0, 1)), d = hash(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
void main() {
    vec2 p = vPos - pc.drift * pc.time;
    float n = 0.7 * vnoise(p / pc.octave1) + 0.3 * vnoise(p / 160.0);
    float dx = min(vPos.x - vRect.x, vRect.z - vPos.x);
    float dy = min(vPos.y - vRect.y, vRect.w - vPos.y);
    float m = smoothstep(0.0, max(vParams.y, 0.001), dx) * smoothstep(0.0, max(vParams.z, 0.001), dy);
    float a = vParams.x * m * mix(0.35, 1.0, n);
    outColor = vec4(pc.tint.rgb, a * pc.tint.a);
}
