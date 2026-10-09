#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* Run the fragment shader, but kill every fragment: the ALU work happens and the
     * PBE receives nothing. Separates shader execution from the PBE write path. */
    color = vec4(1.0);
    discard;
}
