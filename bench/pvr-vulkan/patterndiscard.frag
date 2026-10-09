#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* IDENTICAL ALU work to the pattern shader, then discard: the fragment shader cost
     * is the same as a real render, but nothing reaches the PBE. This is the correct
     * control for separating PBE write cost from shader cost. */
    float r = fract(gl_FragCoord.x / 64.0);
    float g = fract(gl_FragCoord.y / 64.0);
    color = vec4(r, g, 0.25, 1.0);
    discard;
}
