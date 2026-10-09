#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* Pure multiplies ONLY - nothing for the compiler to fuse into fmad. */
    vec4 acc = vec4(gl_FragCoord.xy * 0.001, 0.25, 1.0);
    for (int i = 0; i < 32; i++) {
        acc = acc * vec4(1.0001, 1.0002, 1.0003, 1.0004);
        acc = acc * vec4(0.9999, 0.9998, 0.9997, 0.9996);
    }
    color = acc;
}
