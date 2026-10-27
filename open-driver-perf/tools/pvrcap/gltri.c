/* gltri: headless GLES2 triangle through whatever Mesa driver the loader picks
 * (MESA_LOADER_DRIVER_OVERRIDE=pvr for the native driver). Clears an FBO to green, draws a red
 * triangle with its own VS/FS, reads back and reports pixel counts + an ASCII view.
 * Exit 0 only if the triangle covers about the expected area. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 512
#define H 512

static GLuint sh(GLenum t, const char *src)
{
   GLuint s = glCreateShader(t);
   GLint ok = 0;
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(2); }
   return s;
}

int main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min, n;
   if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 2; }
   static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
   EGLConfig cfg;
   eglBindAPI(EGL_OPENGL_ES_API);
   if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) {
      /* surfaceless may expose no configs: create a config-less context */
      cfg = EGL_NO_CONFIG_KHR;
   }
   static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext c = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
   if (c == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed 0x%x\n", eglGetError()); return 2; }
   printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   GLuint fbo, rb, tex;
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   if (getenv("GLTRI_TEX")) {
      glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      if (getenv("GLTRI_BGRA"))
         glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, W, H, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
      else
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      fprintf(stderr, "after TexImage2D: err 0x%x\n", glGetError());
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   } else {
      glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
      GLint rw = 0; glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &rw);
      fprintf(stderr, "after RenderbufferStorage: err 0x%x width %d\n", glGetError(), rw);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   }
   GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete: status 0x%x, glGetError 0x%x\n", st, glGetError()); return 2; }

   GLuint p = glCreateProgram();
   const int vary = getenv("GLTRI_VARYING") != NULL;
   glAttachShader(p, sh(GL_VERTEX_SHADER, vary
      ? "attribute vec2 pos; varying vec2 v; void main() { v = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }"
      : "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, vary
      ? "precision mediump float; varying vec2 v; void main() { gl_FragColor = vec4(v, 0.0, 1.0); }"
      : getenv("GLTRI_UNIFORM")
      ? "precision mediump float; uniform vec2 rg; void main() { gl_FragColor = vec4(rg, 0.0, 1.0); }"
      : "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   { GLint lk = 0; glGetProgramiv(p, GL_LINK_STATUS, &lk); if (!lk) { char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "link failed: %s\n", log); return 2; } }
   glUseProgram(p);
   if (getenv("GLTRI_UNIFORM")) glUniform2f(glGetUniformLocation(p, "rg"), 1.0f, 0.0f);

   static const float tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
   GLuint vb;
   glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glEnableVertexAttribArray(0);

   glViewport(0, 0, W, H);
   glClearColor(0, 1, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 3);

   static unsigned char px[W * H * 4];
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   printf("glGetError: 0x%x\n", glGetError());
   long red = 0, green = 0, other = 0;
   for (int i = 0; i < W * H; i++) {
      unsigned char *q = px + 4 * i;
      if (q[0] > 200 && q[1] < 50) red++; else if (q[1] > 200 && q[0] < 50) green++; else other++;
   }
   printf("red %ld green %ld other %ld (expected red 32768)\n", red, green, other);
   { unsigned char *q = px + 4 * (200 * W + 256); printf("pixel inside triangle (256,200): %02x %02x %02x %02x\n", q[0], q[1], q[2], q[3]); }
   for (int y = H - 1; y >= 0; y -= 32) { /* glReadPixels row 0 is the bottom: print top first */
      for (int x = 0; x < W; x += 12) {
         unsigned char *q = px + 4 * (y * W + x);
         putchar(q[0] > 200 && q[1] < 50 ? 'R' : q[1] > 200 && q[0] < 50 ? '.' : '?');
      }
      putchar('\n');
   }
   if (vary) {
      /* v = pos * 0.5 + 0.5 is the window position over the size: red = x/W, green = y/H. */
      static const int pts[][2] = { { 256, 200 }, { 200, 150 }, { 300, 160 }, { 256, 300 } };
      int bad = 0;
      for (unsigned i = 0; i < 4; i++) {
         const int x = pts[i][0], y = pts[i][1];
         unsigned char *q = px + 4 * (y * W + x);
         const int er = (int)((x + 0.5f) / W * 255.0f + 0.5f), eg = (int)((y + 0.5f) / H * 255.0f + 0.5f);
         const int ok = abs(q[0] - er) <= 3 && abs(q[1] - eg) <= 3 && q[2] == 0;
         bad += !ok;
         printf("varying (%d,%d): %02x %02x %02x expected %02x %02x 00 %s\n", x, y, q[0], q[1], q[2], er, eg, ok ? "ok" : "BAD");
      }
      return bad ? 1 : 0;
   }
   return (red > 30000 && red < 35500 && green > 220000) ? 0 : 1;
}
