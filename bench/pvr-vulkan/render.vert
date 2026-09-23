#version 450
// Full-screen triangle generated from gl_VertexIndex, so the test needs no vertex
// buffer and no input state: the only thing being exercised is the graphics path
// itself (rasteriser, interpolation, tile buffer, resolve).
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
