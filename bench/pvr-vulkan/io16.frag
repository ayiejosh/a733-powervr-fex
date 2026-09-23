#version 450
// storageInputOutput16: the fragment half. Reads the 16-bit varying and uses it
// for the blue channel, so the expected image is identical to render.frag's
// (0.25 blue) and the existing pixel check applies unchanged.
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require

layout(location = 0) in f16vec4 vcol;
layout(location = 0) out vec4 color;

void main()
{
    float r = fract(gl_FragCoord.x / 64.0);
    float g = fract(gl_FragCoord.y / 64.0);
    color = vec4(r, g, float(vcol.x), 1.0);
}
