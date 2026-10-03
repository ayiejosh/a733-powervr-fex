#version 450
/* Writes into a storage buffer from the VERTEX stage. If the driver honours
 * vertexPipelineStoresAndAtomics these stores land; if it only reports the
 * feature, the buffer comes back untouched. */
layout(std430, binding = 0) buffer Out {
    uint values[];
} outbuf;

layout(location = 0) out vec4 vcolor;

void main()
{
    outbuf.values[gl_VertexIndex] = 100u + uint(gl_VertexIndex);
    gl_Position = vec4(float(gl_VertexIndex) - 1.0, 0.0, 0.0, 1.0);
    vcolor = vec4(1.0);
}
