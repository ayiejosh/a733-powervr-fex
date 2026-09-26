/* Trace the EGL config negotiation a Qt/KWin process performs, to find out which attribute set
 * the vendor EGL cannot satisfy. Motivated by:
 *
 *   $ LD_LIBRARY_PATH=/usr/local/lib kwin_x11 --version
 *   Cannot find EGLConfig, returning null config        <- printed by Qt (libQt6Gui.so.6)
 *
 * and bench/egl-configs.c showing the vendor EGL *does* have ES2/ES3 configs for every X visual
 * including the depth-32 ARGB one, so the mismatch must be in the attribute list Qt asks with.
 *
 * Build: gcc -shared -fPIC -O2 egl-trace.c -o /tmp/egl-trace.so -ldl
 * Run:   LD_PRELOAD=/tmp/egl-trace.so LD_LIBRARY_PATH=/usr/local/lib kwin_x11 --version
 *        (or any Qt app; drop LD_LIBRARY_PATH to trace the system Mesa EGL instead)
 *
 * Hooks both the direct symbol and eglGetProcAddress, because Qt resolves EGL entry points
 * through eglGetProcAddress and would otherwise bypass LD_PRELOAD.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

static PFNEGLCHOOSECONFIGPROC real_choose;
static PFNEGLGETPROCADDRESSPROC real_gpa;
static int depth;

static const char *an(EGLint a){
    switch(a){
    case EGL_SURFACE_TYPE: return "SURFACE_TYPE";
    case EGL_RENDERABLE_TYPE: return "RENDERABLE_TYPE";
    case EGL_RED_SIZE: return "RED";
    case EGL_GREEN_SIZE: return "GREEN";
    case EGL_BLUE_SIZE: return "BLUE";
    case EGL_ALPHA_SIZE: return "ALPHA";
    case EGL_DEPTH_SIZE: return "DEPTH";
    case EGL_STENCIL_SIZE: return "STENCIL";
    case EGL_BUFFER_SIZE: return "BUFFER";
    case EGL_SAMPLES: return "SAMPLES";
    case EGL_SAMPLE_BUFFERS: return "SAMPLE_BUFFERS";
    case EGL_NATIVE_VISUAL_ID: return "NATIVE_VISUAL_ID";
    case EGL_NATIVE_VISUAL_TYPE: return "NATIVE_VISUAL_TYPE";
    case EGL_NATIVE_RENDERABLE: return "NATIVE_RENDERABLE";
    case EGL_CONFIG_CAVEAT: return "CONFIG_CAVEAT";
    case EGL_CONFIG_ID: return "CONFIG_ID";
    case EGL_LEVEL: return "LEVEL";
    case EGL_TRANSPARENT_TYPE: return "TRANSPARENT_TYPE";
    case EGL_BIND_TO_TEXTURE_RGB: return "BIND_TO_TEXTURE_RGB";
    case EGL_BIND_TO_TEXTURE_RGBA: return "BIND_TO_TEXTURE_RGBA";
    case EGL_CONFORMANT: return "CONFORMANT";
    case EGL_COLOR_BUFFER_TYPE: return "COLOR_BUFFER_TYPE";
    case EGL_ALPHA_MASK_SIZE: return "ALPHA_MASK_SIZE";
    case EGL_LUMINANCE_SIZE: return "LUMINANCE_SIZE";
    case EGL_MAX_PBUFFER_WIDTH: return "MAX_PBUFFER_WIDTH";
    case EGL_MAX_PBUFFER_HEIGHT: return "MAX_PBUFFER_HEIGHT";
    case EGL_MAX_PBUFFER_PIXELS: return "MAX_PBUFFER_PIXELS";
    case EGL_MIN_SWAP_INTERVAL: return "MIN_SWAP_INTERVAL";
    case EGL_MAX_SWAP_INTERVAL: return "MAX_SWAP_INTERVAL";
    default: return "?";
    }
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config){
    if(!real_choose) real_choose = (PFNEGLCHOOSECONFIGPROC)dlsym(RTLD_NEXT,"eglChooseConfig");
    if(attrib_list){
        fprintf(stderr,"%*s[egl-trace] eglChooseConfig(", depth*2, "");
        for(int i=0; attrib_list[i]!=EGL_NONE; i+=2)
            fprintf(stderr,"%s%s=%d", i?" ":"", an(attrib_list[i]), attrib_list[i+1]);
        fprintf(stderr,")\n");
    }
    EGLBoolean r = real_choose(dpy,attrib_list,configs,config_size,num_config);
    if(attrib_list)
        fprintf(stderr,"%*s[egl-trace]   -> ok=%d matched=%d%s\n", depth*2, "", r,
                num_config?*num_config:-1, (num_config && *num_config==0)?"   <-- NOTHING MATCHES":"");
    return r;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name){
    if(!real_gpa) real_gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(RTLD_NEXT,"eglGetProcAddress");
    if(name && strcmp(name,"eglChooseConfig")==0) return (__eglMustCastToProperFunctionPointerType)eglChooseConfig;
    return real_gpa(name);
}

/* also bracket the context creation, so a null-config crash shows up in the trace */
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attrs){
    static PFNEGLCREATECONTEXTPROC real_cc;
    if(!real_cc) real_cc = (PFNEGLCREATECONTEXTPROC)dlsym(RTLD_NEXT,"eglCreateContext");
    if(attrs){
        fprintf(stderr,"[egl-trace] eglCreateContext(config=%p):", (void*)cfg);
        for(int i=0; attrs[i]!=EGL_NONE; i+=2)
            fprintf(stderr," %s=%d", an(attrs[i]), attrs[i+1]);
        fprintf(stderr,"\n");
    }
    EGLContext c = real_cc(dpy,cfg,share,attrs);
    fprintf(stderr,"[egl-trace]   -> ctx=%p err=0x%x\n", (void*)c, eglGetError());
    return c;
}
