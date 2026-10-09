#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* HEAVY: real-scene shader cost - many dependent ALU ops, a few transcendentals and a
     * branch, i.e. the shape of a game/compositor fragment shader rather than a fill. */
    vec2 p = gl_FragCoord.xy * 0.001;
    vec4 acc = vec4(p, 0.25, 1.0);
    for (int i = 0; i < 32; i++) {
        acc.x = sin(acc.x * 1.7 + acc.y) * 0.5 + 0.5;
        acc.y = cos(acc.y * 1.3 + acc.z) * 0.5 + 0.5;
        acc.z = fract(acc.x * acc.y + acc.w);
        acc.w = sqrt(abs(acc.z - 0.5)) + 0.5;
        acc = normalize(acc + 0.001);
    }
    color = acc;
}
