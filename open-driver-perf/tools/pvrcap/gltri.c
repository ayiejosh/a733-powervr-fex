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

static int W = 512, H = 512;

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
   if (getenv("GLTRI_W")) W = atoi(getenv("GLTRI_W"));
   if (getenv("GLTRI_H")) H = atoi(getenv("GLTRI_H"));
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
   if (getenv("GLTRI_DEPTH")) {
      GLuint db;
      glGenRenderbuffers(1, &db); glBindRenderbuffer(GL_RENDERBUFFER, db);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
      glEnable(GL_DEPTH_TEST);
   }
   GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete: status 0x%x, glGetError 0x%x\n", st, glGetError()); return 2; }

   GLuint p = glCreateProgram();
   const int vary = getenv("GLTRI_VARYING") != NULL || getenv("GLTRI_SAMPLE") != NULL;
   if (getenv("GLTRI_MAT")) {
      glAttachShader(p, sh(GL_VERTEX_SHADER,
         "attribute vec3 pos; attribute vec3 nrm; uniform mat4 mvp; uniform mat4 nm; varying vec3 c;"
         "void main() { c = (nm * vec4(nrm, 0.0)).xyz * 0.5 + 0.5; gl_Position = mvp * vec4(pos, 1.0); }"));
      glAttachShader(p, sh(GL_FRAGMENT_SHADER,
         "precision mediump float; varying vec3 c; void main() { gl_FragColor = vec4(c, 1.0); }"));
   } else {
   glAttachShader(p, sh(GL_VERTEX_SHADER, vary
      ? "attribute vec2 pos; varying vec2 v; void main() { v = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }"
      : getenv("GLTRI_LOOP") ? "attribute vec2 pos; uniform int n; void main() { float d = 0.25; for (int i = 0; i < n; i++) d = fract(3.0 * d + 0.1); gl_Position = vec4(pos * (1.0 + 0.001 * d), 0.0, 1.0); }"
      : getenv("GLTRI_VSU") ? "attribute vec2 pos; uniform float s; void main() { gl_Position = vec4(pos * s, 0.0, 1.0); }"
      : "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, vary
      ? (getenv("GLTRI_SAMPLE") ? "precision mediump float; varying vec2 v; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, v); }"
                                : "precision mediump float; varying vec2 v; void main() { gl_FragColor = vec4(v, 0.0, 1.0); }")
      : getenv("GLTRI_UNIFORM")
      ? "precision mediump float; uniform vec2 rg; void main() { gl_FragColor = vec4(rg, 0.0, 1.0); }"
      : getenv("GLTRI_BLEND") ? "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 0.5); }"
      : "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
   }
   glBindAttribLocation(p, 0, "pos");
   glBindAttribLocation(p, 0, "pos");
   glBindAttribLocation(p, 1, "nrm");
   glLinkProgram(p);
   { GLint lk = 0; glGetProgramiv(p, GL_LINK_STATUS, &lk); if (!lk) { char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "link failed: %s\n", log); return 2; } }
   glUseProgram(p);
   if (getenv("GLTRI_MAT")) {
      static float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
      if (getenv("GLTRI_PERSP")) id[3] = (float)atof(getenv("GLTRI_PERSP")); /* w = 1 + k * x */
      glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, id);
      glUniformMatrix4fv(glGetUniformLocation(p, "nm"), 1, GL_FALSE, id);
   }
   if (getenv("GLTRI_LOOP")) glUniform1i(glGetUniformLocation(p, "n"), atoi(getenv("GLTRI_LOOP")));
   if (getenv("GLTRI_VSU")) glUniform1f(glGetUniformLocation(p, "s"), 1.0f);
   if (getenv("GLTRI_UNIFORM")) glUniform2f(glGetUniformLocation(p, "rg"), 1.0f, 0.0f);

   const int mat = getenv("GLTRI_MAT") != NULL;
   static const float tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
   /* pos.xyz, nrm.xyz interleaved, like the glmark2 build scene */
   static float tri6[] = { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f };
   if (getenv("GLTRI_Z"))
      sscanf(getenv("GLTRI_Z"), "%f,%f,%f", &tri6[2], &tri6[8], &tri6[14]);
   GLuint vb;
   glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   if (mat) {
      const long ofs = getenv("GLTRI_NOFS") ? atol(getenv("GLTRI_NOFS")) : 0;
      if (ofs) {
         /* positions at 0, normals at ofs (non-interleaved, like glmark2 meshes) */
         float *b = calloc(1, ofs + 36);
         for (int i = 0; i < 3; i++) { memcpy(b + 3 * i, tri6 + 6 * i, 12); memcpy((char *)b + ofs + 12 * i, tri6 + 6 * i + 3, 12); }
         glBufferData(GL_ARRAY_BUFFER, ofs + 36, b, GL_STATIC_DRAW);
         glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, 0);
         glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 12, (void *)ofs);
      } else {
      glBufferData(GL_ARRAY_BUFFER, sizeof tri6, tri6, GL_STATIC_DRAW);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, 0);
      glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
      }
      glEnableVertexAttribArray(1);
   } else {
      glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   }
   glEnableVertexAttribArray(0);

   if (getenv("GLTRI_SAMPLE")) {
      /* 4x4 texels, each a distinct colour */
      unsigned char t[64];
      for (int i = 0; i < 16; i++) { t[4*i] = 16 * i; t[4*i+1] = 255 - 16 * i; t[4*i+2] = (i * 37) & 255; t[4*i+3] = 255; }
      GLuint st;
      glActiveTexture(GL_TEXTURE0);
      glGenTextures(1, &st); glBindTexture(GL_TEXTURE_2D, st);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, getenv("GLTRI_LINEAR") ? GL_LINEAR : GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, getenv("GLTRI_LINEAR") ? GL_LINEAR : GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, st);
   }
   glViewport(0, 0, W, H);
   glClearColor(0, 1, 0, 1);
   if (getenv("GLTRI_CULL")) {
      glEnable(GL_CULL_FACE);
      glCullFace(getenv("GLTRI_CULL")[0] == 'f' ? GL_FRONT : GL_BACK);
      if (getenv("GLTRI_CW")) glFrontFace(GL_CW);
   }
   glClear(GL_COLOR_BUFFER_BIT | (getenv("GLTRI_DEPTH") ? GL_DEPTH_BUFFER_BIT : 0));
   if (getenv("GLTRI_MANY")) {
      /* N small triangles tiled over the left half of the target, then the test triangle on top. */
      const int n = atoi(getenv("GLTRI_MANY")), side = (int)(n > 1 ? __builtin_sqrtf((float)n) + 1 : 1);
      float *v = malloc((size_t)n * 6 * sizeof(float));
      for (int i = 0; i < n; i++) {
         const float x = -1.0f + (float)(i % side) / side, y = -1.0f + 2.0f * (float)(i / side) / side;
         const float d = getenv("GLTRI_BIG") ? 1.5f : 1.0f / side;
         float t[6] = { x, y, x + d, y, x, y + d };
         memcpy(v + i * 6, t, sizeof t);
      }
      GLuint mb;
      glGenBuffers(1, &mb); glBindBuffer(GL_ARRAY_BUFFER, mb);
      glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * 6 * sizeof(float), v, GL_STATIC_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
      glDrawArrays(GL_TRIANGLES, 0, 3 * n);
      glBindBuffer(GL_ARRAY_BUFFER, vb);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   }
   if (getenv("GLTRI_BLEND")) {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   }
   if (!getenv("GLTRI_NODRAW"))
   glDrawArrays(GL_TRIANGLES, 0, 3);
   if (getenv("GLTRI_ZTEST")) {
      /* A bigger triangle behind the first (z 0.5, normal +y): hidden where they overlap. */
      static const float back[] = { -0.9f, -0.9f, 0.5f, 0.0f, 1.0f, 0.0f,
                                     0.9f, -0.9f, 0.5f, 0.0f, 1.0f, 0.0f,
                                     0.0f,  0.9f, 0.5f, 0.0f, 1.0f, 0.0f };
      glBufferData(GL_ARRAY_BUFFER, sizeof back, back, GL_STATIC_DRAW);
      glDrawArrays(GL_TRIANGLES, 0, 3);
   }

   unsigned char *px = malloc((size_t)W * H * 4);
   if (getenv("GLTRI_SUBIMG") && getenv("GLTRI_TEX")) {
      /* Write into the rendered target with the CPU: a 16x16 blue patch at the origin. */
      static unsigned char blue[16 * 16 * 4];
      for (int i = 0; i < 16 * 16; i++) { blue[4*i+2] = 255; blue[4*i+3] = 255; }
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, blue);
   }
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
   if (getenv("GLTRI_SAMPLE")) {
      unsigned long sum = 0;
      for (long i = 0; i < (long)W * H * 4; i++) sum = sum * 31 + px[i];
      unsigned char *q = px + 4 * (200 * W + 256);
      printf("sample checksum %016lx pixel (256,200) %02x %02x %02x %02x\n", sum, q[0], q[1], q[2], q[3]);
      return 0;
   }
   if (getenv("GLTRI_SUBIMG")) {
      unsigned char *q = px + 4 * (5 * W + 5), *r = px + 4 * (200 * W + 256);
      printf("subimg patch (5,5) %02x %02x %02x  triangle (256,200) %02x %02x %02x\n",
             q[0], q[1], q[2], r[0], r[1], r[2]);
      return 0;
   }
   if (getenv("GLTRI_ZTEST")) {
      unsigned char *q = px + 4 * (200 * W + 256), *r = px + 4 * (100 * W + 100);
      printf("ztest front (256,200) %02x %02x %02x  behind-only (100,100) %02x %02x %02x\n",
             q[0], q[1], q[2], r[0], r[1], r[2]);
      return 0;
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
