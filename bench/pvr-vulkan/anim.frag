#version 450
// Animated variant of render.frag: the same predictable pattern, shifted
// horizontally by a push constant so consecutive frames differ in a way the host
// can compute exactly. Separate from render.frag on purpose - vkrender's pipeline
// has no push constant range, and mixing the two would be a layout mismatch.
layout(push_constant) uniform Push {
    float phase;
} pc;

layout(location = 0) out vec4 color;

void main()
{
    float r = fract((gl_FragCoord.x + pc.phase) / 64.0);
    float g = fract(gl_FragCoord.y / 64.0);
    color = vec4(r, g, 0.25, 1.0);
}
