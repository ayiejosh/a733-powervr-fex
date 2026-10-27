#version 450
layout(location = 0) flat in vec4 col;
layout(location = 0) out vec4 o;
void main() { o = col; }
