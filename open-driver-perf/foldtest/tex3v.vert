#version 450
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform sampler2D b[2];
layout(location = 0) flat out vec4 col;
void main() {
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
   vec2 c = vec2(0.5);
   col = vec4(textureLod(a, c, 0.0).r, textureLod(b[0], c, 0.0).r, textureLod(b[1], c, 0.0).r, 1.0);
}
