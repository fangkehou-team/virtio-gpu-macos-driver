#version 450
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=0) out vec3 outNormal;
layout(location=1) out vec3 outColor;
void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    outNormal = inNormal;
    outColor = abs(inNormal) + vec3(0.15, 0.1, 0.1);
}
