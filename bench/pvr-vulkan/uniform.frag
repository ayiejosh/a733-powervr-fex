#version 450
layout(location = 0) out vec4 color;
void main()
{
    /* Uniform colour: ideally compressible if the GPU does frame-buffer compression. */
    color = vec4(0.25, 0.5, 0.75, 1.0);
}
