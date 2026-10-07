#version 450
#define IN(i) layout(location = i) in vec4 v##i;
#if NOUT > 0
IN(0)  IN(1)  IN(2)  IN(3)  IN(4)  IN(5)  IN(6)  IN(7)
#endif
#if NOUT > 8
IN(8)  IN(9)  IN(10) IN(11) IN(12) IN(13) IN(14) IN(15)
#endif
#if NOUT > 16
IN(16) IN(17) IN(18) IN(19) IN(20) IN(21) IN(22) IN(23)
#endif
#if NOUT > 24
IN(24) IN(25) IN(26) IN(27) IN(28) IN(29) IN(30) IN(31)
#endif
layout(location = 0) out vec4 o;
void main() {
    vec4 s = vec4(0.0);
#define ADD(i) s += v##i;
#if NOUT > 0
    ADD(0)  ADD(1)  ADD(2)  ADD(3)  ADD(4)  ADD(5)  ADD(6)  ADD(7)
#endif
#if NOUT > 8
    ADD(8)  ADD(9)  ADD(10) ADD(11) ADD(12) ADD(13) ADD(14) ADD(15)
#endif
#if NOUT > 16
    ADD(16) ADD(17) ADD(18) ADD(19) ADD(20) ADD(21) ADD(22) ADD(23)
#endif
#if NOUT > 24
    ADD(24) ADD(25) ADD(26) ADD(27) ADD(28) ADD(29) ADD(30) ADD(31)
#endif
    /* s.x must equal 0+1+...+(NOUT-1); scale by 1/512 so the value lands in [0,1)
     * for every count tested (max sum 496 at NOUT=32) and survives an RGBA8 attachment. */
    o = vec4(s.x / 512.0, 0.0, 0.0, 1.0);
}
