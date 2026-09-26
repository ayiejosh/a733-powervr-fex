#version 450
// Depth clamp probe: the same full-screen triangle as render.vert, but at
// z = 2.0 with w = 1.0, which is outside the clip volume.
//
// With depthClampEnable = VK_FALSE every fragment is clipped away and the render
// target keeps its clear colour. With depthClampEnable = VK_TRUE the Z is clamped
// to maxDepth and the triangle is drawn, so the image is render.frag's pattern,
// unchanged - which is why the existing pixel check applies to the clamped case
// verbatim.
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 2.0, 1.0);
}
