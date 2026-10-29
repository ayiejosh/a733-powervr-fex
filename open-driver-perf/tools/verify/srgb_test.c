/* sRGB: EXT_sRGB texture sampling (decode) and render target (PBE gamma encode), compared with zink.
 * Sampling: upload sRGB byte 128 -> linear 0.2158 -> written to an RGBA8 target -> ~55.
 * Render target: clear linear 0.5 / draw linear 0.25 -> stored sRGB -> decoded on sample -> 128/64. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_SRGB_EXT
#define GL_SRGB_EXT 0x8C40
#define GL_SRGB_ALPHA_EXT 0x8C42
#endif
static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x,1,&s,0); glCompileShader(x); return x; }
static GLuint prog(const char *vs, const char *fs) {
   GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(p);
   GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { char l[512]; glGetProgramInfoLog(p, 512, 0, l); fprintf(stderr, "link: %s\n", l); }
   return p;
}
static void sample_into(GLuint tex, const char *what, int *out) {
   const char *vs = "attribute vec2 p; varying vec2 v; void main(){ v = p*0.5+0.5; gl_Position = vec4(p,0,1); }";
   const char *fs = "precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }";
   static GLuint pt, rd, fbo;
   static int init;
   if (!init) {
      pt = prog(vs, fs);
      glGenTextures(1, &rd); glBindTexture(GL_TEXTURE_2D, rd);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenFramebuffers(1, &fbo); init = 1;
   }
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rd, 0);
   glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(pt); GLint l = glGetAttribLocation(pt, "p");
   glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, (float[]){ -3,-1, 1,-1, 1,3 });
   glEnableVertexAttribArray(l);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
   glUniform1i(glGetUniformLocation(pt, "t"), 0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   unsigned char px[4] = { 0 };
   glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   fprintf(stderr, "%s: %u %u %u %u err 0x%x\n", what, px[0], px[1], px[2], px[3], glGetError());
   if (out) { out[0] = px[0]; out[1] = px[1]; out[2] = px[2]; out[3] = px[3]; }
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
   const char *ext = (const char *)glGetString(GL_EXTENSIONS);
   fprintf(stderr, "EXT_sRGB=%s\n", ext && strstr(ext, "sRGB") ? "yes" : "no");
   /* 1) Sample an uploaded sRGB texture: byte 128 is sRGB-encoded 0.2158 linear. */
   GLuint tex;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   unsigned char src[64*64*4];
   for (int i = 0; i < 64*64; i++) { src[4*i+0]=128; src[4*i+1]=128; src[4*i+2]=128; src[4*i+3]=255; }
   glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB_ALPHA_EXT, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   sample_into(tex, "sRGB texture decode (expect ~55 55 55 255)", NULL);
   /* 2) Render into an sRGB target: linear 0.5/0.25 must encode on write and decode on sample. */
   GLuint rt, fbo;
   glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB_ALPHA_EXT, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
   fprintf(stderr, "sRGB fbo status 0x%x err 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER), glGetError());
   glViewport(0,0,64,64);
   glClearColor(0.5f, 0.5f, 0.5f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glFinish();
   sample_into(rt, "sRGB rt clear 0.5 (expect 128 128 128 255)", NULL);
   const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p,0,1); }";
   const char *fs = "precision mediump float; void main(){ gl_FragColor = vec4(0.25, 0.25, 0.25, 1.0); }";
   GLuint p = prog(vs, fs); glUseProgram(p); GLint l = glGetAttribLocation(p, "p");
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, (float[]){ -1,-1, 3,-1, -1,3 });
   glEnableVertexAttribArray(l);
   glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
   sample_into(rt, "sRGB rt draw 0.25 (expect 64 64 64 255)", NULL);
   return 0;
}
