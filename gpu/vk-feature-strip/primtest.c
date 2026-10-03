/*
 * primtest — draw one OpenGL primitive type through whatever GL driver is
 * configured, and say whether it survived.
 *
 * One primitive per process on purpose: the PowerVR blob aborts the whole
 * process when it is asked to compile a geometry shader, so a single run
 * cannot report on more than one mode.
 *
 *   primtest quads|quad_strip|polygon|line_loop|triangles
 *
 * Uses EGL on an X11 window - the path this board's desktop actually uses,
 * since the X server exports no GLX at all and a pbuffer/surfaceless context
 * does not survive screen creation here.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLenum pick(const char *n)
{
    if (!strcmp(n, "quads"))      return GL_QUADS;
    if (!strcmp(n, "quad_strip")) return GL_QUAD_STRIP;
    if (!strcmp(n, "polygon"))    return GL_POLYGON;
    if (!strcmp(n, "line_loop"))  return GL_LINE_LOOP;
    if (!strcmp(n, "triangles"))  return GL_TRIANGLES;
    return 0;
}

/* Sum the RGB of every pixel. With additive blending an overlapping second
 * instance doubles the value, so this separates 1 instance from 2. */
static long pixel_sum(void)
{
    unsigned char buf[64 * 64 * 4];
    long s = 0;
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    for (int i = 0; i < 64 * 64; i++)
        s += buf[i * 4] + buf[i * 4 + 1] + buf[i * 4 + 2];
    return s;
}

/* Count non-black pixels in the 64x64 window. */
static int lit_pixels(void)
{
    unsigned char buf[64 * 64 * 4];
    int n = 0;
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    for (int i = 0; i < 64 * 64; i++)
        if (buf[i * 4] || buf[i * 4 + 1] || buf[i * 4 + 2]) n++;
    return n;
}

static const GLfloat verts[5][2] = {
    { -0.9f, -0.9f }, { 0.9f, -0.9f }, { 0.0f, 0.9f },
    { -0.5f,  0.2f }, { 0.5f, 0.2f },
};

/* Four vertices per topology, so triangles / strip / fan all produce 2 tris. */
static const GLubyte idx_tris[6] = { 0, 1, 2, 0, 2, 3 };
static const GLubyte idx_strip[4] = { 0, 1, 2, 3 };
static const GLubyte idx_fan[4]   = { 2, 0, 1, 3 };

static void draw_arrays(GLenum mode, int n)
{
    glDrawArrays(mode, 0, n);
}

static void draw_elements(GLenum mode, const GLubyte *idx, int n)
{
    glDrawElements(mode, n, GL_UNSIGNED_BYTE, idx);
}

/* Does glPolygonMode(GL_LINE) actually change anything? That is what
 * VkPhysicalDeviceFeatures.fillModeNonSolid buys, so this probes the capability
 * rather than the report.
 *
 * Runs the full matrix: every triangle topology, drawn both as vertex arrays and
 * through an index buffer, plus a multi-draw - because the line expansion in zink
 * handles each of those differently.
 */
static int wireframe_probe(void)
{
    struct { const char *name; GLenum mode; int n; const GLubyte *idx; } cases[] = {
        { "tri arrays",   GL_TRIANGLES,      3, NULL },
        { "tri indexed",  GL_TRIANGLES,      6, idx_tris },
        { "strip arrays", GL_TRIANGLE_STRIP, 4, NULL },
        { "strip indexed",GL_TRIANGLE_STRIP, 4, idx_strip },
        { "fan arrays",   GL_TRIANGLE_FAN,   4, NULL },
        { "fan indexed",  GL_TRIANGLE_FAN,   4, idx_fan },
    };
    int bad = 0;

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glClearColor(0, 0, 0, 1);
    glColor3f(1, 1, 1);

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int filled, lined, ok;

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        if (cases[c].idx) draw_elements(cases[c].mode, cases[c].idx, cases[c].n);
        else              draw_arrays(cases[c].mode, cases[c].n);
        glFinish();
        filled = lit_pixels();

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        if (cases[c].idx) draw_elements(cases[c].mode, cases[c].idx, cases[c].n);
        else              draw_arrays(cases[c].mode, cases[c].n);
        glFinish();
        lined = lit_pixels();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        ok = lined > 0 && lined < filled / 2;
        if (!ok) bad++;
        printf("  %-14s filled=%-5d lined=%-5d -> %s\n", cases[c].name, filled, lined,
               ok ? "wireframe WORKS" : "wireframe IGNORED");
    }

    /* multi-draw: two separate triangle ranges in one call */
    {
        const GLint first[2] = { 0, 3 };
        const GLsizei count[2] = { 3, 2 };
        int filled, lined, ok;

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glMultiDrawArrays(GL_TRIANGLES, first, count, 2);
        glFinish();
        filled = lit_pixels();

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glMultiDrawArrays(GL_TRIANGLES, first, count, 2);
        glFinish();
        lined = lit_pixels();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        ok = lined > 0 && lined < filled / 2;
        if (!ok) bad++;
        printf("  %-14s filled=%-5d lined=%-5d -> %s\n", "multidraw", filled, lined,
               ok ? "wireframe WORKS" : "wireframe IGNORED");
    }

    /* Primitive restart: a strip with a restart index must produce exactly the
     * same lines as the same strip without one. If the expansion walks across
     * the break it draws extra edges and the counts differ. */
    {
        static const GLubyte plain[3]   = { 0, 1, 2 };
        static const GLubyte broken[7]  = { 0, 1, 2, 9, 0, 1, 2 };
        int a, b;

        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_elements(GL_TRIANGLE_STRIP, plain, 3);
        glFinish();
        a = lit_pixels();

        glPrimitiveRestartIndexNV(9);
        glEnableClientState(GL_PRIMITIVE_RESTART_NV);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_elements(GL_TRIANGLE_STRIP, broken, 7);
        glFinish();
        b = lit_pixels();
        glDisableClientState(GL_PRIMITIVE_RESTART_NV);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        int ok = a > 0 && a == b;
        if (!ok) bad++;
        printf("  %-14s plain=%-5d with-restart=%-5d -> %s\n", "restart", a, b,
               ok ? "handled" : "EDGE DRAWN ACROSS THE BREAK");
    }

    /* Polygon point mode: each polygon vertex becomes a point. A filled triangle
     * covers ~1682 pixels; three points cover far fewer, so the counts separate
     * cleanly. */
    {
        int filled, pointed;

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        draw_arrays(GL_TRIANGLES, 3);
        glFinish();
        filled = lit_pixels();

        glClear(GL_COLOR_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
        draw_arrays(GL_TRIANGLES, 3);
        glFinish();
        pointed = lit_pixels();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        int ok = pointed > 0 && pointed < filled / 2;
        if (!ok) bad++;
        printf("  %-14s filled=%-5d points=%-5d -> %s\n", "point mode", filled, pointed,
               ok ? "points WORK" : "point mode IGNORED");
    }

    /* Instancing: the expansion copies instance_count to the line draw, so two
     * instances must draw twice the geometry. Additive blending makes the
     * overlap measurable. */
    {
        long one, two;

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        /* Dim, or the first draw already sits at 255 and adding cannot show. */
        glColor3f(0.25f, 0.25f, 0.25f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArraysInstancedARB(GL_TRIANGLES, 0, 3, 1);
        glFinish();
        one = pixel_sum();

        /* control: the same triangle drawn twice with two ordinary calls. If this
         * does not double either, additive blending is what is not working. */
        glClear(GL_COLOR_BUFFER_BIT);
        draw_arrays(GL_TRIANGLES, 3);
        draw_arrays(GL_TRIANGLES, 3);
        glFinish();
        printf("  instanced      control (2 plain draws)=%ld\n", pixel_sum());

        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArraysInstancedARB(GL_TRIANGLES, 0, 3, 2);
        glFinish();
        two = pixel_sum();

        glDisable(GL_BLEND);
        glColor3f(1, 1, 1);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        int ok = one > 0 && two > one * 3 / 2;
        if (!ok) bad++;
        printf("  %-14s 1 instance=%-7ld 2 instances=%-7ld -> %s\n", "instanced", one, two,
               ok ? "instancing WORKS" : "SECOND INSTANCE LOST");
    }

    /* Line stipple applies to the lines the wireframe expansion generates, which
     * is what glPolygonMode(GL_LINE) should do. A stippled line covers fewer
     * pixels than a solid one. */
    {
        int solid, stippled;

        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_arrays(GL_TRIANGLES, 3);
        glFinish();
        solid = lit_pixels();

        glEnable(GL_LINE_STIPPLE);
        glLineStipple(1, 0x00FF);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_arrays(GL_TRIANGLES, 3);
        glFinish();
        stippled = lit_pixels();
        glDisable(GL_LINE_STIPPLE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        /* Not counted as a failure: this driver implements no stippled line mode
         * and has no primitive-level stage to emulate it with, so a dropped
         * stipple is the documented state rather than a regression. */
        int ok = solid > 0 && stippled < solid;
        printf("  %-14s solid=%-5d stippled=%-5d -> %s\n", "stipple", solid, stippled,
               ok ? "stipple APPLIES" : "stipple ignored (known limit)");
    }

    glDisableClientState(GL_VERTEX_ARRAY);
    return bad ? 1 : 0;
}

/* Transform feedback must capture the triangles the app asked for, not the lines
 * the wireframe expansion generates. The expansion is skipped while TF is active;
 * this checks the capture really is intact.
 */
static int tf_probe(void)
{
    static const char *vs =
        "#version 120\n"
        "varying vec4 tf_out;\n"
        "void main() {\n"
        "  tf_out = gl_Vertex;\n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "}\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    GLint ok = 0;
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(v, sizeof(log) - 1, NULL, log);
        printf("  %-14s SKIP: vertex shader did not compile: %s\n", "tf + wireframe", log);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    const char *varyings[1] = { "tf_out" };
    glTransformFeedbackVaryings(prog, 1, varyings, GL_SEPARATE_ATTRIBS);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
        printf("  %-14s SKIP: program did not link: %s\n", "tf + wireframe", log);
        return 0;
    }

    while (glGetError() != GL_NO_ERROR) { }
    {
        GLint sep = -1, inter = -1, maxidx = -1;
        glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, &sep);
        glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, &inter);
        glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, &maxidx);
        /* The limits do not even resolve here: the only transform-feedback
         * extension this stack advertises is _overflow_query, which is not
         * transform feedback. So TF cannot be exercised at all. */
        if (glGetError() != GL_NO_ERROR || sep <= 0 || inter <= 0) {
            printf("  %-14s SKIP: no transform feedback on this stack\n", "tf + wireframe");
            return 0;
        }
    }
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, 4096, NULL, GL_STREAM_READ);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buf);
    glUseProgram(prog);

    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_TRIANGLES);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);   /* wireframe while capturing */
    draw_arrays(GL_TRIANGLES, 3);
    glEndTransformFeedback();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_RASTERIZER_DISCARD);
    glUseProgram(0);

    /* 3 vertices x vec4 = 12 floats. More means the line expansion leaked into
     * the capture, which would be silent corruption of the app's data. */
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    const float *m = (const float *)glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    int floats = 0;
    if (m) {
        for (int i = 0; i < 128; i++) {
            if (m[i] != 0.0f) floats = i + 1;
        }
        glUnmapBuffer(GL_ARRAY_BUFFER);
    }

    int good = (floats == 12);
    printf("  %-14s captured %d floats (expected 12 = 3 verts) -> %s\n",
           "tf + wireframe", floats, good ? "capture intact" : "CAPTURE CORRUPTED");

    glDeleteBuffers(1, &buf);
    glDeleteProgram(prog);
    glDeleteShader(v);
    return good ? 0 : 1;
}

/* Point-mode polygon rasterisation on its own, so it can be run in isolation
 * many times: a single run once reported a wildly different pixel count and the
 * cause was never found. */
static int points_probe(void)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glClearColor(0, 0, 0, 1);
    glColor3f(1, 1, 1);

    glClear(GL_COLOR_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    draw_arrays(GL_TRIANGLES, 3);
    glFinish();
    int filled = lit_pixels();

    glClear(GL_COLOR_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    draw_arrays(GL_TRIANGLES, 3);
    glFinish();
    int pointed = lit_pixels();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* control: a plain GL_POINTS draw, no polygon mode involved at all. If this
     * is also unstable then points are broken on this stack generally, and
     * emulating polygon point mode cannot be made correct. */
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(1.0f);
    draw_arrays(GL_POINTS, 3);
    glFinish();
    int plain = lit_pixels();

    /* an explicitly large point size: if this is respected and stable, the
     * flakiness is only in the size-1 path. 3 points at 8x8 ~ 192 px. */
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(8.0f);
    draw_arrays(GL_POINTS, 3);
    glFinish();
    int big = lit_pixels();
    glPointSize(1.0f);

    GLfloat ps = 0; glGetFloatv(GL_POINT_SIZE, &ps);
    GLint vp[4] = {0}; glGetIntegerv(GL_VIEWPORT, vp);
    printf("filled=%d points=%d plain1=%d plain8=%d ps=%.1f vp=%dx%d\n",
           filled, pointed, plain, big, ps, vp[2], vp[3]);
    glDisableClientState(GL_VERTEX_ARRAY);
    return (pointed > 0 && pointed < filled / 2) ? 0 : 1;
}

/* glEdgeFlag: an edge is drawn only if the edge flag of its first vertex is
 * TRUE. With flags (T,F,F) on a triangle only one of the three edges should
 * appear, so the pixel count must drop against all-TRUE. */
static int edgeflag_probe(void)
{
    static const GLboolean one_edge[3] = { GL_TRUE, GL_FALSE, GL_FALSE };
    static const GLboolean all_edges[3] = { GL_TRUE, GL_TRUE, GL_TRUE };

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glEnableClientState(GL_EDGE_FLAG_ARRAY);
    glClearColor(0, 0, 0, 1);
    glColor3f(1, 1, 1);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    glEdgeFlagPointer(0, all_edges);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_arrays(GL_TRIANGLES, 3);
    glFinish();
    int full = lit_pixels();

    glEdgeFlagPointer(0, one_edge);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_arrays(GL_TRIANGLES, 3);
    glFinish();
    int one = lit_pixels();

    /* Indexed variant: the flag is looked up by vertex index, resolved through
     * the index buffer, so this exercises a different path than the arrays case.
     * Two triangles over vertices 0..3: six edges when all flags are set, and
     * two when only vertex 0 is flagged. */
    static const GLboolean idx_all[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    static const GLboolean idx_one[4] = { GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE };
    int ifull = 0, ione = 0;

    glEdgeFlagPointer(0, idx_all);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_elements(GL_TRIANGLES, idx_tris, 6);
    glFinish();
    ifull = lit_pixels();

    glEdgeFlagPointer(0, idx_one);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_elements(GL_TRIANGLES, idx_tris, 6);
    glFinish();
    ione = lit_pixels();

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    printf("all-TRUE=%d  (T,F,F)=%d  indexed all=%d one=%d  -> %s\n", full, one, ifull, ione,
           (one > 0 && one < full && ione > 0 && ione < ifull) ? "edge flags APPLY"
                                                               : "edge flags IGNORED");
    return (one > 0 && one < full && ione > 0 && ione < ifull) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: primtest quads|quad_strip|polygon|line_loop|triangles\n");
        return 2;
    }
    int is_wireframe = !strcmp(argv[1], "wireframe");
    int is_tf = !strcmp(argv[1], "tf") || !strcmp(argv[1], "points") || !strcmp(argv[1], "edgeflag");
    GLenum mode = pick(argv[1]);
    if (!mode && !is_wireframe && !is_tf) { printf("FAIL unknown mode %s\n", argv[1]); return 2; }

    Display *xdpy = XOpenDisplay(NULL);
    if (!xdpy) { printf("FAIL XOpenDisplay (DISPLAY=%s)\n", getenv("DISPLAY")); return 1; }

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
    if (dpy == EGL_NO_DISPLAY) { printf("FAIL eglGetDisplay\n"); return 1; }
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { printf("FAIL eglInitialize\n"); return 1; }

    EGLint cfgattr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        printf("FAIL no EGL config for EGL_OPENGL_BIT + window\n");
        return 1;
    }

    int scr = DefaultScreen(xdpy);
    Window win = XCreateSimpleWindow(xdpy, RootWindow(xdpy, scr), 0, 0, 64, 64, 0,
                                     BlackPixel(xdpy, scr), BlackPixel(xdpy, scr));
    XMapWindow(xdpy, win);
    XSync(xdpy, False);

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
    if (surf == EGL_NO_SURFACE) { printf("FAIL eglCreateWindowSurface (0x%x)\n", eglGetError()); return 1; }

    eglBindAPI(EGL_OPENGL_API);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext (0x%x)\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("FAIL eglMakeCurrent (0x%x)\n", eglGetError()); return 1; }

    printf("  renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("  version : %s\n", (const char *)glGetString(GL_VERSION));

    if (is_wireframe)
        return wireframe_probe();
    if (!strcmp(argv[1], "tf"))
        return tf_probe();
    if (!strcmp(argv[1], "points"))
        return points_probe();
    if (!strcmp(argv[1], "edgeflag"))
        return edgeflag_probe();

    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(mode);
    glColor3f(1, 0, 0); glVertex2f(-0.5f, -0.5f);
    glColor3f(0, 1, 0); glVertex2f( 0.5f, -0.5f);
    glColor3f(0, 0, 1); glVertex2f( 0.5f,  0.5f);
    glColor3f(1, 1, 0); glVertex2f(-0.5f,  0.5f);
    glEnd();
    glFinish();

    GLenum e = glGetError();
    printf("%s %s\n", e == GL_NO_ERROR ? "OK" : "FAIL glGetError", argv[1]);
    return e == GL_NO_ERROR ? 0 : 1;
}
