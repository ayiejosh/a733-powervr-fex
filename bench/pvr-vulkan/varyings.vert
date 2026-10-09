#version 450
#define OUT(i) layout(location = i) out vec4 v##i;
#if NOUT > 0
OUT(0)  OUT(1)  OUT(2)  OUT(3)  OUT(4)  OUT(5)  OUT(6)  OUT(7)
#endif
#if NOUT > 8
OUT(8)  OUT(9)  OUT(10) OUT(11) OUT(12) OUT(13) OUT(14) OUT(15)
#endif
#if NOUT > 16
OUT(16) OUT(17) OUT(18) OUT(19) OUT(20) OUT(21) OUT(22) OUT(23)
#endif
#if NOUT > 24
OUT(24) OUT(25) OUT(26) OUT(27) OUT(28) OUT(29) OUT(30) OUT(31)
#endif
void main() {
    /* full-screen triangle from gl_VertexIndex - no vertex buffer needed */
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
#define SET(i) v##i = vec4(float(i));
#if NOUT > 0
    SET(0)  SET(1)  SET(2)  SET(3)  SET(4)  SET(5)  SET(6)  SET(7)
#endif
#if NOUT > 8
    SET(8)  SET(9)  SET(10) SET(11) SET(12) SET(13) SET(14) SET(15)
#endif
#if NOUT > 16
    SET(16) SET(17) SET(18) SET(19) SET(20) SET(21) SET(22) SET(23)
#endif
#if NOUT > 24
    SET(24) SET(25) SET(26) SET(27) SET(28) SET(29) SET(30) SET(31)
#endif
}
