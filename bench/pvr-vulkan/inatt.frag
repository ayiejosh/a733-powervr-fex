#version 450
#define IN(i) layout(input_attachment_index = i, set = 0, binding = i) uniform subpassInput ia##i;
#if NIN > 0
IN(0) IN(1) IN(2) IN(3)
#endif
#if NIN > 4
IN(4) IN(5) IN(6) IN(7)
#endif
#if NIN > 8
IN(8) IN(9) IN(10) IN(11)
#endif
#if NIN > 12
IN(12) IN(13) IN(14) IN(15)
#endif
layout(location = 0) out vec4 o;
void main() {
    vec4 s = vec4(0.0);
#define ADD(i) s += subpassLoad(ia##i);
#if NIN > 0
    ADD(0) ADD(1) ADD(2) ADD(3)
#endif
#if NIN > 4
    ADD(4) ADD(5) ADD(6) ADD(7)
#endif
#if NIN > 8
    ADD(8) ADD(9) ADD(10) ADD(11)
#endif
#if NIN > 12
    ADD(12) ADD(13) ADD(14) ADD(15)
#endif
    o = vec4(s.r, 0.0, 0.0, 1.0);
}
