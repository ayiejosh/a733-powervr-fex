/*
 * glheadless.c - can the open stack deliver desktop GL, with no display server?
 *
 * The live desktop gets GL through zink (GL -> Vulkan) over the *vendor* Vulkan
 * ICD. This asks the same question one layer open: zink over Mesa's pvr Vulkan
 * driver on the mainline kernel driver. To keep the two comparable it renders the
 * same pattern as vkrender.c - two 64-pixel ramps plus a constant - so the pixels
 * and the throughput can be read against the Vulkan numbers directly.
 *
 * No window: EGL_MESA_platform_surfaceless plus an FBO, which is all the driver
 * needs to be exercised. Tries desktop GL (3.3 core) first and falls back to
 * GLES 3, because a zink build can be either.
 *
 * Build: see build.sh   Run: EGL_PLATFORM=surfaceless ./glheadless [size] [iters]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#if defined(__has_include)
#  if __has_include(<GLES3/gl3.h>)
#    include <GLES3/gl3.h>   /* superset of GLES2; provides GL_RGBA8 */
#  else
#    include <GLES2/gl2.h>
#    ifndef GL_RGBA8
#      define GL_RGBA8 0x8058
#    endif
#  endif
#else
#  include <GLES2/gl2.h>
#endif

#define DIE(...)                               \
    do {                                       \
        fprintf(stderr, "FAIL: " __VA_ARGS__); \
        fputc('\n', stderr);                   \
        exit(1);                               \
    } while (0)

static const char *VERT_GL =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_GL =
    "#version 330 core\n"
    "layout(location = 0) out vec4 color;\n"
    "void main() {\n"
    "    float r = fract(gl_FragCoord.x / 64.0);\n"
    "    float g = fract(gl_FragCoord.y / 64.0);\n"
    "    color = vec4(r, g, 0.25, 1.0);\n"
    "}\n";

static const char *VERT_ES2 =
    "#version 100\n"
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *FRAG_ES2 =
    "#version 100\n"
    "precision mediump float;\n"
    "void main() {\n"
    "    float r = fract(gl_FragCoord.x / 64.0);\n"
    "    float g = fract(gl_FragCoord.y / 64.0);\n"
    "    gl_FragColor = vec4(r, g, 0.25, 1.0);\n"
    "}\n";

static const char *VERT_ES =
    "#version 300 es\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_ES =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    float r = fract(gl_FragCoord.x / 64.0);\n"
    "    float g = fract(gl_FragCoord.y / 64.0);\n"
    "    color = vec4(r, g, 0.25, 1.0);\n"
    "}\n";

static int expect_r(int v)
{
    double f = (v + 0.5) / 64.0;
    f -= floor(f);
    return (int)lround(fmin(fmax(f, 0.0), 1.0) * 255.0);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}


/* EGL config selection that tolerates platforms which do not expose
 * pbuffer-renderable configs (GBM, surfaceless) and drivers that differ on alpha. */
static int pick_config(EGLDisplay d, EGLint renderable_bit, const char *what, EGLConfig *out)
{
    const EGLint surface_types[3] = { EGL_PBUFFER_BIT, EGL_WINDOW_BIT, 0 };
    const char *surface_names[3] = { "pbuffer", "window", "none" };

    for (int st = 0; st < 3; st++) {
        for (int alpha = 8; alpha >= 0; alpha -= 8) {
            EGLint attrs[] = {
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, alpha,
                EGL_RENDERABLE_TYPE, renderable_bit,
                EGL_SURFACE_TYPE, surface_types[st],
                EGL_NONE,
            };
            EGLint n = 0;
            if (eglChooseConfig(d, attrs, out, 1, &n) && n > 0) {
                printf("%s: config found (surface=%s alpha=%d)\n", what, surface_names[st],
                       alpha);
                return 1;
            }
        }
    }
    printf("%s: no config\n", what);
    return 0;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log) - 1, &n, log);
        log[n] = 0;
        DIE("shader compile failed: %s", log);
    }
    return s;
}

int main(int argc, char **argv)
{
    uint32_t size = 512;
    int iters = 20;

    if (argc > 1)
        size = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        iters = atoi(argv[2]);
    if (size < 64 || iters < 1)
        DIE("bad arguments");

    /* ---- EGL, surfaceless ------------------------------------------------ */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_platform_display)
        DIE("no eglGetPlatformDisplayEXT (need a Mesa EGL)");

    /* Two headless ways in. "device" (EGL_EXT_platform_device) picks the DRM
     * device explicitly instead of letting Mesa iterate its own device list,
     * which is the more robust path when several drivers are installed. */
    const char *want = getenv("EGL_PLATFORM");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (want && strcmp(want, "gbm") == 0) {
        /* The GBM platform loads its gallium driver through LIBGL_DRIVERS_PATH,
         * which is a different code path from the DRI2 screen setup. Useful when
         * the DRI2 path cannot find a driver for the kernel's DRM name. */
        const char *node = getenv("DRM_RENDER_NODE");
        if (!node)
            DIE("EGL_PLATFORM=gbm needs DRM_RENDER_NODE");
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            DIE("cannot open %s: %s", node, strerror(errno));
        struct gbm_device *gbm = gbm_create_device(fd);
        if (!gbm)
            DIE("gbm_create_device(%s) failed", node);
        printf("gbm device created on %s\n", node);
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_pd =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        dpy = get_pd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    } else if (want && strcmp(want, "device") == 0) {
        PFNEGLQUERYDEVICESEXTPROC query_devices =
            (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string =
            (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
        const char *want_node = getenv("DRM_RENDER_NODE");
        EGLDeviceEXT devs[16];
        EGLint ndev = 0;
        if (!query_devices || !query_device_string || !query_devices(16, devs, &ndev) || ndev == 0)
            DIE("no EGL devices enumerated (need EGL_EXT_device_enumeration)");
        printf("EGL devices: %d\n", ndev);
        for (EGLint i = 0; i < ndev; i++) {
            const char *node = query_device_string(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
            printf("  device[%d] render node: %s\n", i, node ? node : "(none)");
            if (want_node && node && strcmp(node, want_node) == 0)
                dpy = get_platform_display(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        }
        if (dpy == EGL_NO_DISPLAY)
            DIE("no EGL device matching DRM_RENDER_NODE=%s", want_node ? want_node : "(unset)");
    } else {
        dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (dpy == EGL_NO_DISPLAY)
        DIE("eglGetPlatformDisplay failed");

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        EGLint err = eglGetError();
        DIE("eglInitialize failed (0x%x)", err);
    }
    printf("EGL %d.%d  vendor=%s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

    /* GLES, not desktop GL: this binary links libGLESv2, so the entry points it
     * calls are the ES ones. The vendor baseline (231.7 Mpix/s) was measured the
     * same way, which also makes the two comparable. */
    static EGLConfig cfg;
    int use_es = 1;
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        DIE("eglBindAPI(GLES) failed");
    /* Ask for the ES3 bit first: EGL_OPENGL_ES3_BIT_KHR is 0x40 and a config
     * advertising only 0x1|0x4|0x8 will refuse an ES3 context with EGL_BAD_CONFIG
     * ("context api is 0x40 while config supports 0xd"). */
    if (!pick_config(dpy, EGL_OPENGL_ES3_BIT_KHR, "GLES 3", &cfg) &&
        !pick_config(dpy, EGL_OPENGL_ES2_BIT, "GLES 2", &cfg))
        DIE("no EGL config - zink initialised but EGL cannot offer one");

    EGLint chosen_rt = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &chosen_rt);
    printf("config renderable=0x%x (GL=%d ES2=%d)\n", chosen_rt,
           !!(chosen_rt & EGL_OPENGL_BIT), !!(chosen_rt & EGL_OPENGL_ES2_BIT));

    /* Context creation is fussy about which attribute set a driver accepts, and
     * the reason for a rejection is not observable, so try the plausible sets in
     * order and report which one worked. */
    EGLint es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLint es3_alt[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    struct {
        const char *name;
        EGLint *attrs;
        int es_version;
    } tries[] = { { "GLES 3 (client version)", es3, 3 },
                  { "GLES 3 (major/minor)", es3_alt, 3 },
                  { "GLES 2", es2, 2 } };

    EGLContext ctx = EGL_NO_CONTEXT;
    int ctx_es_version = 0;
    for (unsigned i = 0; i < sizeof(tries) / sizeof(tries[0]) && ctx == EGL_NO_CONTEXT; i++) {
        /* Bind per attempt: EGL's "current API" decides which renderable bit the
         * context is checked against, and a stale OpenGL binding makes an
         * ES-only config reject the context (or worse, accept it as GL). */
        eglBindAPI(EGL_OPENGL_ES_API);
        printf("  pre-create: eglQueryAPI=0x%x (ES=0x%x, GL=0x%x)\n", eglQueryAPI(),
               EGL_OPENGL_ES_API, EGL_OPENGL_API);
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, tries[i].attrs);
        printf("  context %-24s -> %s (egl error 0x%x)\n", tries[i].name,
               ctx != EGL_NO_CONTEXT ? "OK" : "rejected", eglGetError());
        if (ctx != EGL_NO_CONTEXT)
            ctx_es_version = tries[i].es_version;
    }
    if (ctx == EGL_NO_CONTEXT)
        DIE("no context attribute set was accepted (last error 0x%x)", eglGetError());
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        DIE("eglMakeCurrent(surfaceless) failed (0x%x)", eglGetError());
    /* GLES 2 is fine: it needs a vertex buffer and gl_FragColor, nothing else. */
    int use_es2 = (ctx_es_version < 3);
    if (use_es2)
        printf("using the GLES 2 shader path\n");
    printf("EGL_CLIENT_APIS=%s current_api=0x%x ctx=%p\n",
           eglQueryString(dpy, EGL_CLIENT_APIS), eglQueryAPI(), (void *)eglGetCurrentContext());
    printf("GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));

    /* ---- FBO + program --------------------------------------------------- */
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        DIE("FBO incomplete (0x%x)", glCheckFramebufferStatus(GL_FRAMEBUFFER));

    GLuint vs = compile(GL_VERTEX_SHADER, use_es2 ? VERT_ES2 : VERT_ES);
    GLuint fs = compile(GL_FRAGMENT_SHADER, use_es2 ? FRAG_ES2 : FRAG_ES);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    if (use_es2)
        glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked)
        DIE("program link failed");
    glUseProgram(prog);
    glViewport(0, 0, size, size);

    if (use_es2) {
        /* Full-screen triangle, same positions the ES3 shader derives from
         * gl_VertexID, so the expected pixels are identical. */
        static const float verts[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    unsigned char *px = malloc((size_t)size * size * 4);
    if (!px)
        DIE("out of memory for readback");

    printf("rendering %d x %ux%u frames (FBO + glReadPixels)...\n", iters, size, size);
    /* Split the frame: how much is the draw being submitted, how much is the
     * readback, how much is the implicit flush. */
    int timing = getenv("PVR_TIMING") != NULL;
    double t_draw = 0, t_read = 0, t_finish = 0;
    double t0 = now_ms();
    for (int it = 0; it < iters; it++) {
        double _a = now_ms();
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        double _b = now_ms();
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, px);
        double _c = now_ms();
        glFinish();
        double _d = now_ms();
        if (timing) {
            t_draw += _b - _a;
            t_read += _c - _b;
            t_finish += _d - _c;
        }
    }
    double t1 = now_ms();

    uint64_t bad = 0;
    uint32_t fx = 0, fy = 0;
    int er = 0, eg = 0, gr = 0, gg = 0;
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            /* glReadPixels returns rows bottom-up; the shader uses GL's own
             * gl_FragCoord origin, so the expectation follows the same origin. */
            const unsigned char *p = px + ((size_t)y * size + x) * 4;
            int wr = expect_r(x), wg = expect_r(y);
            if (p[0] != wr || p[1] != wg || p[2] != 64 || p[3] != 255) {
                if (bad == 0) {
                    fx = x; fy = y; er = wr; eg = wg; gr = p[0]; gg = p[1];
                }
                bad++;
            }
        }
    }

    double ms = t1 - t0;
    if (timing) {
        double n = iters > 0 ? iters : 1;
        printf("timing ms/frame: gl_calls=%.3f readpixels=%.3f glFinish=%.3f (sum=%.3f of %.3f)\n",
               t_draw / n, t_read / n, t_finish / n, (t_draw + t_read + t_finish) / n, ms / n);
    }
    printf("%d frame(s) in %.3f ms (%.3f ms/frame, %.1f Mpix/s)\n", iters, ms, ms / iters,
           (double)size * size * iters / (ms / 1000.0) / 1e6);
    if (bad)
        printf("RESULT: FAIL - %llu/%u pixels wrong (first at %u,%u: want r=%d g=%d got r=%d g=%d)\n",
               (unsigned long long)bad, size * size, fx, fy, er, eg, gr, gg);
    else
        printf("RESULT: PASS - %u/%u pixels correct\n", size * size, size * size);
    printf("VERDICT: %s\n", bad ? "FAIL" : "PASS");

    return bad ? 1 : 0;
}
