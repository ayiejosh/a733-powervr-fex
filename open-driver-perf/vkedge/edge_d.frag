#version 450
layout(location = 0) out vec4 o;
/* dFdx(gl_FragCoord.x) is 1.0 everywhere, so the colour is the same as edge_c,
 * but computing it needs helper invocations in partially covered 2x2 quads. */
void main() { float d = dFdx(gl_FragCoord.x); o = vec4(1.0, 0.0, 1.0, d > 0.5 ? 1.0 : 0.0); }
