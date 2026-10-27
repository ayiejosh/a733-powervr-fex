#version 450
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform sampler2D b[2];
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
   vec2 c = uv * 0.25 + 0.25;
   o = vec4(texture(a, c).r, texture(b[0], c).r, texture(b[1], c).r, 1.0);
}
