#version 450
layout(location = 0) in vec2 inPos;     // pixels
layout(location = 1) in vec4 inRect;    // x0 y0 x1 y1, pixels
layout(location = 2) in vec3 inParams;  // alpha, soft edge x, soft edge y
layout(push_constant) uniform PC { vec2 viewport; float time; float pad0; vec2 drift; vec2 pad1; vec4 tint; } pc;
layout(location = 0) out vec2 vPos;
layout(location = 1) flat out vec4 vRect;
layout(location = 2) flat out vec3 vParams;
void main() {
    vPos = inPos; vRect = inRect; vParams = inParams;
    gl_Position = vec4(inPos / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
}
