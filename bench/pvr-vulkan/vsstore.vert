#version 450
// vertexPipelineStoresAndAtomics: render.vert's full-screen triangle, plus a
// storage-buffer store and an atomic add *from the vertex stage*. The fragment
// stage is render.frag, so the image - and the 262144-pixel check - are
// unchanged; the host checks the buffer afterwards.
//
// Neither write is conditional on the vertex index, so a pass does not depend on
// how many times the hardware chose to run the vertex shader.
layout(set = 0, binding = 0, std430) buffer Out {
    uint marker;
    uint counter;
} o;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    o.marker = 0xABCD1234u;
    atomicAdd(o.counter, 3u);
}
