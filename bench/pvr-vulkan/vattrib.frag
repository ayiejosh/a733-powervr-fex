#version 450
layout(location = 0) in vec4 s;
layout(location = 0) out vec4 o;
void main() { o = vec4(s.x, 0.0, 0.0, 1.0); }
