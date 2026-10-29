/* Minimal R8/RG8 render-target check: clear, then read back in the native single-channel format. */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
int main(void) {
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "no egl\n"); return 2; }
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n = 0;
   EGLConfig cfg = EGL_NO_CONFIG_KHR;
   EGLint attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
   eglChooseConfig(dpy, attr, &cfg, 1, &n);
   EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "no ctx\n"); return 2; }
   for (int rg = 0; rg < 2; rg++) {
      GLuint tex, fbo;
      glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, rg ? GL_RG8_EXT : GL_R8_EXT, 64, 64, 0,
                   rg ? GL_RG_EXT : GL_RED_EXT, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
      fprintf(stderr, "rt%d status 0x%x\n", rg ? 8 : 1, glCheckFramebufferStatus(GL_FRAMEBUFFER));
      glViewport(0, 0, 64, 64);
      glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glFinish();
      unsigned char px[2] = { 0 };
      if (rg) glReadPixels(32, 32, 1, 1, GL_RG_EXT, GL_UNSIGNED_BYTE, px);
      else    glReadPixels(32, 32, 1, 1, GL_RED_EXT, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "rt%d readback: %u %u (err 0x%x)\n", rg ? 8 : 1, px[0], px[1], glGetError());
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex);
   }
   return 0;
}
