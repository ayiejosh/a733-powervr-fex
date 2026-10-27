#version 450
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform UB { vec4 col; } u;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(textureLod(a, uv * 0.25 + 0.25, 0.0).r, u.col.g, u.col.b, 1.0); }
