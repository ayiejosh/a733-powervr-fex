#version 450
// A pattern the host can predict exactly, pixel for pixel, in the 8-bit output
// format: two 64-pixel ramps plus a constant. gl_FragCoord is the pixel centre,
// so the host evaluates the same expression at (x + 0.5, y + 0.5).
layout(location = 0) out vec4 color;

void main()
{
    float r = fract(gl_FragCoord.x / 64.0);
    float g = fract(gl_FragCoord.y / 64.0);
    color = vec4(r, g, 0.25, 1.0);
}
