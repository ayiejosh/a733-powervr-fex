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
#include <GL/gl.h>
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: primtest quads|quad_strip|polygon|line_loop|triangles\n");
        return 2;
    }
    GLenum mode = pick(argv[1]);
    if (!mode) { printf("FAIL unknown mode %s\n", argv[1]); return 2; }

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
