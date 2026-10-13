#version 450
layout(location=0) in vec3 inNormal;
layout(location=1) in vec3 inColor;
layout(location=0) out vec4 outColor;
void main() {
    float d = max(dot(normalize(inNormal), normalize(vec3(0.3, 0.5, 0.8))), 0.0) * 0.7 + 0.3;
    outColor = vec4(inColor * d, 1.0);
}
