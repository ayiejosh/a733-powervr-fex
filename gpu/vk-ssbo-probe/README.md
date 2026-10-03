# vk-ssbo-probe

Does this driver *honour* `vertexPipelineStoresAndAtomics`, or only report it?

A vertex shader writes `100 + gl_VertexIndex` into a storage buffer, a triangle is
drawn, and the buffer is read back. This matters because it is the enabling
capability for emulating transform feedback without `VK_EXT_transform_feedback`:
the feedback varyings can be stored to a buffer from the vertex stage, indexed by
vertex, which keeps the ordering deterministic.

Deliberately minimal - no vertex buffer, and a 4x4 render target that exists only
because Vulkan demands one.

    glslangValidator -V probe.vert -o probe.vert.spv
    glslangValidator -V probe.frag -o probe.frag.spv
    gcc -O2 -o probe probe.c -lvulkan
    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./probe

Measured:

| run | buffer | verdict |
|---|---|---|
| PowerVR BXM-4-64 | `100 101 102` | stores land |
| same, with the store deleted from the shader | `0 0 0` | probe reports DROPPED, so it discriminates |
| lavapipe | `100 101 102` | sanity on a second driver |
