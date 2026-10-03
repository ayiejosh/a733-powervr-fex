/* Does transform feedback actually capture on this driver?
 *
 * Emulated TF stores the captured varyings from the vertex shader through a
 * device address, so the only way to know it works is to ask GL for the data
 * back and look at it.
 *
 * A vertex shader passes gl_Vertex straight to a varying, three vertices are
 * drawn with rasterisation discarded and feedback active, and the buffer is
 * read back. Positions (1,2,3,4) (5,6,7,8) (9,10,11,12) must come out in that
 * order - order included, because the emulation indexes by vertex rather than
 * relying on atomics.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_TRANSFORM_FEEDBACK_BUFFER_EXT
#define GL_TRANSFORM_FEEDBACK_BUFFER_EXT 0x8C8E
#endif
#ifndef GL_INTERLEAVED_ATTRIBS_EXT
#define GL_INTERLEAVED_ATTRIBS_EXT 0x8C8C
#endif
#ifndef GL_RASTERIZER_DISCARD_EXT
#define GL_RASTERIZER_DISCARD_EXT 0x8C89
#endif

static const GLfloat verts[3][4] = {
    { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 },
};

int main(void)
{
    Display *xdpy = XOpenDisplay(NULL);
    if (!xdpy) { printf("FAIL XOpenDisplay\n"); return 1; }

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
    if (dpy == EGL_NO_DISPLAY) { printf("FAIL eglGetDisplay\n"); return 1; }
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { printf("FAIL eglInitialize\n"); return 1; }

    EGLint cfgattr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        printf("FAIL no EGL config\n"); return 1;
    }

    int scr = DefaultScreen(xdpy);
    Window win = XCreateSimpleWindow(xdpy, RootWindow(xdpy, scr), 0, 0, 64, 64, 0,
                                     BlackPixel(xdpy, scr), BlackPixel(xdpy, scr));
    XMapWindow(xdpy, win);
    XSync(xdpy, False);

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
    if (surf == EGL_NO_SURFACE) { printf("FAIL eglCreateWindowSurface\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    EGLContext ectx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (ectx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext\n"); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ectx)) { printf("FAIL eglMakeCurrent\n"); return 1; }

    printf("  renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("  version : %s\n", (const char *)glGetString(GL_VERSION));

    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_EXT_transform_feedback")) {
        printf("RESULT: GL_EXT_transform_feedback not exposed - nothing to test\n");
        return 0;
    }
    printf("  GL_EXT_transform_feedback: exposed\n");

    static const char *vs_src =
        "#version 120\n"
        "varying vec4 out_val;\n"
        "void main() {\n"
        "  out_val = gl_Vertex;\n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "}\n";
    static const char *fs_src =
        "#version 120\n"
        "void main() { gl_FragColor = vec4(1.0); }\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512] = {0}; glGetShaderInfoLog(vs, 511, NULL, log);
               printf("FAIL vertex shader: %s\n", log); return 1; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    const char *varyings[1] = { "out_val" };
    glTransformFeedbackVaryingsEXT(prog, 1, varyings, GL_INTERLEAVED_ATTRIBS_EXT);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512] = {0}; glGetProgramInfoLog(prog, 511, NULL, log);
               printf("FAIL link: %s\n", log); return 1; }
    glUseProgram(prog);

    GLuint tfb = 0;
    glGenBuffers(1, &tfb);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, tfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 256, NULL, GL_STREAM_READ);
    glBindBufferBaseEXT(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0, tfb);
    printf("  glGetError after setup: 0x%x\n", glGetError());

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, verts);
    glEnable(GL_RASTERIZER_DISCARD_EXT);

    glBeginTransformFeedbackEXT(GL_TRIANGLES);
    printf("  glGetError after begin: 0x%x\n", glGetError());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndTransformFeedbackEXT();
    printf("  glGetError after end:   0x%x\n", glGetError());

    glDisable(GL_RASTERIZER_DISCARD_EXT);
    glDisableClientState(GL_VERTEX_ARRAY);
    glUseProgram(0);

    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, tfb);
    const GLfloat *m = (const GLfloat *)glMapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, GL_READ_ONLY);
    if (!m) { printf("FAIL glMapBuffer returned NULL (0x%x)\n", glGetError()); return 1; }

    printf("  captured:");
    for (int i = 0; i < 12; i++) printf(" %.0f", m[i]);
    printf("\n");
    glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT);

    int good = 1;
    for (int i = 0; i < 12; i++)
        if (m[i] != (GLfloat)(i + 1)) good = 0;

    if (good)
        printf("RESULT: transform feedback CAPTURES correctly, in order\n");
    else {
        int all_zero = 1;
        for (int i = 0; i < 12; i++) if (m[i] != 0.0f) all_zero = 0;
        printf("RESULT: %s\n", all_zero ? "NOTHING CAPTURED - buffer stayed zero"
                                        : "captured, but the values are WRONG");
    }
    return good ? 0 : 1;
}
