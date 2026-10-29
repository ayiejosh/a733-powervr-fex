/* ES2 instanced drawing: 3 instances of a small triangle, per-instance offset (vec2) and colour
 * (vec4) with divisor 1, per-vertex position with divisor 0. Reads one pixel per instance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
typedef void (*PFN_GVAD)(GLuint, GLuint);
typedef void (*PFN_DAI)(GLenum, GLint, GLsizei, GLsizei);
typedef void (*PFN_DEI)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x,1,&s,0); glCompileShader(x); return x; }
static GLuint prog(const char *vs, const char *fs) {
   GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(p);
   GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { char l[512]; glGetProgramInfoLog(p, 512, 0, l); fprintf(stderr, "link: %s\n", l); }
   return p;
}
int main(void) {
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(dpy, 0, 0); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n = 0, a[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
   EGLConfig cfg; eglChooseConfig(dpy, a, &cfg, 1, &n);
   EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext c = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
   if (!eglMakeCurrent(dpy, 0, 0, c)) { fprintf(stderr, "no ctx\n"); return 2; }
   PFN_GVAD div = (PFN_GVAD)eglGetProcAddress("glVertexAttribDivisorEXT");
   PFN_DAI dai = (PFN_DAI)eglGetProcAddress("glDrawArraysInstancedEXT");
   if (!div) div = (PFN_GVAD)eglGetProcAddress("glVertexAttribDivisorNV");
   if (!dai) dai = (PFN_DAI)eglGetProcAddress("glDrawArraysInstancedNV");
   fprintf(stderr, "divisor=%p instanced=%p ext=%s\n", (void *)div, (void *)dai,
           (const char *)glGetString(GL_EXTENSIONS) && strstr((const char *)glGetString(GL_EXTENSIONS), "draw_instanced") ? "yes" : "no");
   if (!div || !dai) return 3;
   GLuint tex, fbo;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   const char *vs = getenv("INST_REORDER")
      ? "attribute vec4 col; attribute vec2 off; attribute vec2 p; varying vec4 v;"
        "void main(){ v = col; gl_Position = vec4(p * 0.4 + off, 0.0, 1.0); }"
      : "attribute vec2 p; attribute vec2 off; attribute vec4 col; varying vec4 v;"
        "void main(){ v = col; gl_Position = vec4(p * 0.4 + off, 0.0, 1.0); }";
   const char *fs = getenv("INST_WHITE") ? "precision mediump float; void main(){ gl_FragColor = vec4(1.0); }"
                                         : "precision mediump float; varying vec4 v; void main(){ gl_FragColor = v; }";
   GLuint p = prog(vs, fs);
   glUseProgram(p);
   glViewport(0, 0, 128, 128);
   glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   GLint lp = glGetAttribLocation(p, "p"), lo = glGetAttribLocation(p, "off"), lc = glGetAttribLocation(p, "col");
   /* Buffers (VBOs): per-vertex triangle, then one element each for the divisor-1 arrays. */
   GLuint bo[3];
   glGenBuffers(3, bo);
   glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
   glBufferData(GL_ARRAY_BUFFER, 6 * 4, (float[]){ -0.5f,-0.5f, 0.5f,-0.5f, 0.0f,0.5f }, GL_STATIC_DRAW);
   glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glEnableVertexAttribArray(lp); div(lp, 0);
   glBindBuffer(GL_ARRAY_BUFFER, bo[1]);
   glBufferData(GL_ARRAY_BUFFER,
                getenv("INST_MAP") ? 8 * 4 : 6 * 4,
                getenv("INST_MAP") ? (float[]){ -0.6f,0.0f, -0.2f,0.0f, 0.2f,0.0f, 0.6f,0.0f }
                : getenv("INST_DIV") ? (float[]){ -0.6f,0.0f, 0.6f,0.0f, 0.0f,0.0f }
                                   : (float[]){ -0.6f,0.0f, 0.0f,0.0f, 0.6f,0.0f }, GL_STATIC_DRAW);
   const int divn = getenv("INST_DIV") ? atoi(getenv("INST_DIV")) : 1;
   glVertexAttribPointer(lo, 2, GL_FLOAT, GL_FALSE, 8, 0);
   glEnableVertexAttribArray(lo); div(lo, divn);
   glBindBuffer(GL_ARRAY_BUFFER, bo[2]);
   glBufferData(GL_ARRAY_BUFFER,
                getenv("INST_MAP") ? 16 * 4 : 12 * 4,
                getenv("INST_MAP") ? (float[]){ 1,0,0,1, 0,1,0,1, 0,0,1,1, 1,1,0,1 }
                : getenv("INST_DIV") ? (float[]){ 1,0,0,1,  0,1,0,1,  0,0,1,1 }
                                   : (float[]){ 1,0,0,1,  0,1,0,1,  0,0,1,1 }, GL_STATIC_DRAW);
   glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 16, 0);
   glEnableVertexAttribArray(lc); div(lc, divn);
   fprintf(stderr, "setup err 0x%x\n", glGetError());
   const int ninst = getenv("INST_N") ? atoi(getenv("INST_N")) : (getenv("INST_MAP") || getenv("INST_DIV") ? 4 : 3);
   PFN_DEI dei = (PFN_DEI)eglGetProcAddress("glDrawElementsInstancedEXT");
   if (getenv("INST_IDX") && dei) {
      static const unsigned short idx[3] = { 0, 1, 2 };
      dei(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx, ninst);
   } else {
      dai(GL_TRIANGLES, 0, 3, ninst);
   }
   glFinish();
   fprintf(stderr, "draw err 0x%x\n", glGetError());
   if (getenv("INST_SCAN")) {
      for (int x = 0; x < 128; x += 4) {
         unsigned char p4[4] = { 0 };
         glReadPixels(x, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p4);
         if (p4[0] || p4[1] || p4[2] || p4[3] != 255)
            fprintf(stderr, "scan x=%3d: %3u %3u %3u %3u\n", x, p4[0], p4[1], p4[2], p4[3]);
      }
   }
   const int xs[4] = { 26, 51, 77, 102 };
   for (int i = 0; i < ninst && i < 4; i++) {
      const int xr = getenv("INST_MAP") ? xs[i]
                    : i == 0 ? 26 : i == 1 ? 64 : 102;
      unsigned char px[4] = { 0 };
      glReadPixels(xr, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "instance%d at x=%d: %u %u %u %u (expect %s)\n", i, xr, px[0], px[1], px[2], px[3],
              i == 0 ? "255 0 0" : i == 1 ? "0 255 0" : "0 0 255");
   }
   return 0;
}
