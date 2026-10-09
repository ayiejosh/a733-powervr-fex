#version 450
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D tex;
void main()
{
    /* Sample the same texture the fill path only writes to: this is exactly the access
     * pattern weston's composite pass uses on the client's linear swapchain image. */
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(tex, 0));
    color = texture(tex, uv);
}
