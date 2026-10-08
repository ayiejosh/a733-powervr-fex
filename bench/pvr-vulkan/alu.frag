#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* Pure ALU: same iteration count as heavy.frag but NO transcendentals
     * (no sin/cos/sqrt/normalize) - only mul/add/fma/min/max/fract. */
    vec4 acc = vec4(gl_FragCoord.xy * 0.001, 0.25, 1.0);
    for (int i = 0; i < 32; i++) {
        acc = acc * 1.0001 + vec4(0.001, 0.002, 0.003, 0.004);
        acc = min(acc, vec4(1.0));
        acc = max(acc, vec4(0.0));
        acc.x = fract(acc.x + acc.y);
        acc.y = acc.z * acc.w + acc.x;
        acc.z = acc.w * 0.5 + acc.y;
        acc.w = acc.x * 0.5 + acc.z;
    }
    color = acc;
}
