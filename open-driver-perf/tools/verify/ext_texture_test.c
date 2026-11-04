/* ES2 extension checks: OES_standard_derivatives, EXT_texture_type_2_10_10_10_REV, OES_texture_float.
 * Each mode renders a full-screen quad and reads the centre pixel, for comparison with zink. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#endif
static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x,1,&s,0); glCompileShader(x);
   GLint ok=0; glGetShaderiv(x, GL_COMPILE_STATUS, &ok); if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile: %s\n",l);} return x; }
static GLuint prog(const char *vs, const char *fs) { GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER,vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER,fs)); glLinkProgram(p);
   GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok); if(!ok){char l[512];glGetProgramInfoLog(p,512,0,l);fprintf(stderr,"link: %s\n",l);} return p; }
static const char *VS = "attribute vec2 p; varying vec2 v; void main(){ v = p*0.5+0.5; gl_Position = vec4(p,0,1); }";
static void quad(void) {
   static int init; GLuint bo;
   if (!init) { glGenBuffers(1,&bo); glBindBuffer(GL_ARRAY_BUFFER,bo);
      glBufferData(GL_ARRAY_BUFFER, 12*4, (float[]){ -1,-1, 3,-1, -1,3 }, GL_STATIC_DRAW); init = 1; }
}
static void run(const char *name, const char *fs) {
   GLuint tex, fbo;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   GLuint p = prog(VS, fs); glUseProgram(p);
   quad(); glBindBuffer(GL_ARRAY_BUFFER, 0);
   const float verts[8] = { -1,-1, 3,-1, -1,3 };
   GLint l = glGetAttribLocation(p, "p");
   glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(l);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   unsigned char px[4] = {0};
   glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   fprintf(stderr, "%s: %u %u %u %u err 0x%x\n", name, px[0], px[1], px[2], px[3], glGetError());
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
   fprintf(stderr, "derivatives=%d tex1010102=%d texfloat=%d\n",
           ext && strstr(ext, "GL_OES_standard_derivatives") != NULL,
           ext && strstr(ext, "GL_EXT_texture_type_2_10_10_10_REV") != NULL,
           ext && strstr(ext, "GL_OES_texture_float") != NULL);
   /* 1) Derivatives: dv/dx over a 64-pixel-wide quad is 1/64, so 64*d is 1.0. */
   run("derivatives (expect 255 255 0 255)",
       "#extension GL_OES_standard_derivatives : enable\n"
       "precision mediump float; varying vec2 v;"
       "void main(){ gl_FragColor = vec4(dFdx(v.x)*64.0, dFdy(v.y)*64.0, 0.0, 1.0); }");
   /* 2) 2_10_10_10 packed texture: (1023, 512, 256, 3) -> (255, 128, 64, 255). */
   {
      GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      const unsigned v = 1023u | (512u << 10) | (256u << 20) | (3u << 30);
      unsigned data[64*64]; for (int i = 0; i < 64*64; i++) data[i] = v;
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, data);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GLuint rt, fbo; glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
      glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
      GLuint p = prog(VS, "precision mediump float; varying vec2 v; uniform sampler2D t;"
                          "void main(){ gl_FragColor = texture2D(t, v); }");
      glUseProgram(p);
      const float verts[8] = { -1,-1, 3,-1, -1,3 };
      GLint l = glGetAttribLocation(p, "p");
      glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(l);
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
      glUniform1i(glGetUniformLocation(p, "t"), 0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      unsigned char px[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "1010102 sample (expect 255 128 64 255): %u %u %u %u err 0x%x\n", px[0], px[1], px[2], px[3], glGetError());
   }
   /* 3) RGBA32F texture: (0.25, 0.5, 0.75, 1.0) -> (64, 128, 191, 255). */
   {
      GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      float data[64*64*4]; for (int i = 0; i < 64*64; i++) { data[4*i]=0.25f; data[4*i+1]=0.5f; data[4*i+2]=0.75f; data[4*i+3]=1.0f; }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_FLOAT, data);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GLuint rt, fbo; glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
      glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
      GLuint p = prog(VS, "precision mediump float; varying vec2 v; uniform sampler2D t;"
                          "void main(){ gl_FragColor = texture2D(t, v); }");
      glUseProgram(p);
      const float verts[8] = { -1,-1, 3,-1, -1,3 };
      GLint l = glGetAttribLocation(p, "p");
      glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(l);
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
      glUniform1i(glGetUniformLocation(p, "t"), 0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      unsigned char px[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "rgba32f sample (expect 64 128 191 255): %u %u %u %u err 0x%x\n", px[0], px[1], px[2], px[3], glGetError());
   }
   return 0;
}
