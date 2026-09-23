#version 450
// storageInputOutput16: a 16-bit varying from the vertex stage to the fragment
// stage. Same full-screen triangle as render.vert, so the host's pixel
// expectation is unchanged - the only difference is that the blue channel
// travels through an f16vec4 varying instead of being a constant.
//
// The value comes from a push constant rather than a literal, for two reasons:
// a literal would be folded into both shaders and the varying would disappear,
// and every vertex has to send the *same* value so the interpolated result is
// that value exactly whatever the barycentrics are.
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require

layout(push_constant) uniform PC {
    float16_t k;
} pc;

layout(location = 0) out f16vec4 vcol;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vcol = f16vec4(pc.k, float16_t(0.0), float16_t(0.0), float16_t(0.0));
}
