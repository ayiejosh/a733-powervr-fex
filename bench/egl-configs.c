/* Enumerate the EGL configs the vendor PowerVR EGL actually offers on X11, and test the
 * attribute sets a compositor asks for. Motivated by KWin 6.3 logging
 * "Cannot find EGLConfig, returning null config" on every start and then segfaulting when
 * OpenGL compositing is enabled (see docs/GPU-RESEARCH-2026-09-22.md §7).
 *
 * Build: gcc -O2 egl-configs.c -o /tmp/egl-configs -lX11 -lEGL
 * Run:   LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 /tmp/egl-configs
 *        (drop LD_LIBRARY_PATH to compare against the system Mesa EGL)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>

static int q(EGLDisplay d, EGLConfig c, EGLint a){ EGLint v=-1; eglGetConfigAttrib(d,c,a,&v); return v; }

static void dump(EGLDisplay d, EGLConfig *cfgs, int n, const char *label){
    printf("\n=== %s ===\n", label);
    printf("id  visual  buf  r  g  b  a  depth stencil samples  surface      renderable\n");
    for(int i=0;i<n;i++){
        EGLConfig c=cfgs[i];
        EGLint st=q(d,c,EGL_SURFACE_TYPE), rt=q(d,c,EGL_RENDERABLE_TYPE);
        printf("%3d %6d %4d %2d %2d %2d %2d %5d %6d %6d   %-12s %s%s%s%s\n",
            q(d,c,EGL_CONFIG_ID), q(d,c,EGL_NATIVE_VISUAL_ID), q(d,c,EGL_BUFFER_SIZE),
            q(d,c,EGL_RED_SIZE), q(d,c,EGL_GREEN_SIZE), q(d,c,EGL_BLUE_SIZE), q(d,c,EGL_ALPHA_SIZE),
            q(d,c,EGL_DEPTH_SIZE), q(d,c,EGL_STENCIL_SIZE), q(d,c,EGL_SAMPLES),
            (st&EGL_WINDOW_BIT)?"WINDOW":"-",
            (rt&EGL_OPENGL_ES2_BIT)?"ES2 ":"", (rt&EGL_OPENGL_ES3_BIT)?"ES3 ":"",
            (rt&EGL_OPENGL_BIT)?"GL ":"", (rt&EGL_PBUFFER_BIT)?"PB ":"");
    }
}

static void try(EGLDisplay d, const char *label, EGLint *attrs){
    EGLConfig c; EGLint n=0;
    EGLBoolean ok=eglChooseConfig(d,attrs,&c,1,&n);
    printf("  %-46s -> ok=%d matched=%d%s\n", label, ok, n, n?"":"   <-- NO CONFIG");
}

/* Which X visuals can the GPU actually render to? A compositor needs a depth-32 ARGB visual;
 * Qt's EGL chooser filters the config list by EGL_NATIVE_VISUAL_ID == the window's visual, so a
 * visual with no matching config is exactly the "Cannot find EGLConfig" case. */
static void visuals(EGLDisplay d){
    Display *x=XOpenDisplay(NULL);
    if(!x){ printf("\n(no X display: skipping the visual cross-reference)\n"); return; }
    XVisualInfo tmpl; int n=0;
    XVisualInfo *list=XGetVisualInfo(x, VisualNoMask, &tmpl, &n);
    printf("\n=== X visuals (%d) vs the EGL configs that can target them ===\n", n);
    printf("visual  depth  class        ARGB   matching configs\n");
    int noegl=0, argb_noegl=0;
    for(int i=0;i<n;i++){
        XVisualInfo *v=&list[i];
        EGLint attrs[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                         EGL_NATIVE_VISUAL_ID,(EGLint)v->visualid, EGL_NONE };
        EGLConfig c; EGLint m=0;
        eglChooseConfig(d,attrs,&c,1,&m);
        const char *cls = v->class==TrueColor?"TrueColor":(v->class==DirectColor?"DirectColor":"other");
        printf("0x%02lx    %2d     %-12s %-5s  %d%s\n", v->visualid, v->depth, cls,
               v->depth==32?"yes":"no", m, m?"":"   <-- NO EGL CONFIG");
        if(!m){ noegl++; if(v->depth==32) argb_noegl++; }
    }
    printf("summary: %d of %d visuals have no EGL config", noegl, n);
    if(argb_noegl) printf(", including %d depth-32 (ARGB) visual(s) -- a compositor has nothing to bind to", argb_noegl);
    printf("\n");
    XFree(list);
}

int main(void){
    Display *x=XOpenDisplay(NULL);
    if(!x){ fprintf(stderr,"no X display\n"); return 2; }
    EGLDisplay d=eglGetDisplay((EGLNativeDisplayType)x);
    EGLint maj,min;
    if(d==EGL_NO_DISPLAY || !eglInitialize(d,&maj,&min)){ fprintf(stderr,"eglInitialize failed\n"); return 1; }
    printf("EGL %d.%d  vendor=%s  apis=%s\n", maj,min,
           eglQueryString(d,EGL_VENDOR), eglQueryString(d,EGL_CLIENT_APIS));
    printf("EGL_PLATFORM_X11=%s  EGL_KHR_platform_gbm=%s\n",
           strstr(eglQueryString(d,EGL_EXTENSIONS),"EGL_EXT_platform_x11")?"yes":"no",
           strstr(eglQueryString(d,EGL_EXTENSIONS),"EGL_KHR_platform_gbm")?"yes":"no");

    EGLint total=0; eglGetConfigs(d,NULL,0,&total);
    EGLConfig *all=malloc(sizeof(EGLConfig)*(total?total:1));
    eglGetConfigs(d,all,total,&total);
    int nwin=0, nalpha=0, nes2=0;
    for(int i=0;i<total;i++){
        if(q(d,all[i],EGL_SURFACE_TYPE)&EGL_WINDOW_BIT) nwin++;
        if(q(d,all[i],EGL_ALPHA_SIZE)>=8) nalpha++;
        if(q(d,all[i],EGL_RENDERABLE_TYPE)&EGL_OPENGL_ES2_BIT) nes2++;
    }
    printf("\nTOTAL configs=%d   window-capable=%d   alpha>=8=%d   ES2-renderable=%d\n",
           total,nwin,nalpha,nes2);
    dump(d,all,total,"all configs");

    printf("\n=== what a compositor asks for ===\n");
    EGLint a1[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8, EGL_NONE };
    EGLint a2[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
                  EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8, EGL_NONE };
    EGLint a3[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_BUFFER_SIZE,32,EGL_ALPHA_SIZE,8, EGL_NONE };
    EGLint a4[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8, EGL_NONE };
    EGLint a5[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_NONE };
    try(d,"minimal RGB888 + WINDOW + ES2",a1);
    try(d,"RGB888 + ALPHA8 + depth24 + stencil8",a2);
    try(d,"BUFFER_SIZE=32 + ALPHA8",a3);
    try(d,"desktop OpenGL (not ES) + WINDOW",a4);
    try(d,"any window config at all",a5);
    visuals(d);
    return 0;
}
