/* rttest: verify the native driver's twiddled+TFBC colour-target path without GBM or CPU twiddle.
 * A pattern is rendered into FBO A (twiddled+TFBC by default, or linear with PVR_NO_TWIDDLE=1),
 * then sampled from A into FBO B, which PVR_FORCE_LINEAR_RT=1 keeps linear, and B is read back.
 * Exit 0 only if every pixel of B matches the pattern that was written into A. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SIZE 512
static GLuint sh(GLenum t, const char *src) {
   GLuint s = glCreateShader(t); GLint ok = 0;
   glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(2); }
   return s;
}
static const char *vs_src =
   "attribute vec2 pos; varying vec2 v_uv;"
   " void main() { v_uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }";
static const char *fs1_src =
   "precision highp float; varying vec2 v_uv; void main() {"
   " float x = floor(v_uv.x * 512.0); float y = floor(v_uv.y * 512.0);"
   " gl_FragColor = vec4(mod(x, 256.0) / 255.0, mod(y, 256.0) / 255.0, 0.0, 1.0); }";
static const char *fs2_src =
   "precision highp float; varying vec2 v_uv; uniform sampler2D tex;"
   " void main() { gl_FragColor = texture2D(tex, v_uv); }";
static GLuint make_target(GLuint *fbo) {
   GLuint tex;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "fbo incomplete\n"); exit(2);
   }
   return tex;
}
int main(void) {
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "egl init failed\n"); return 2; }
   eglBindAPI(EGL_OPENGL_ES_API);
   static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, xattr);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "ctx failed 0x%x\n", eglGetError()); return 2;
   }
   printf("R=%s\nV=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   GLuint fbo_a, fbo_b;
   GLuint tex_a = make_target(&fbo_a);  /* first render target: twiddled+TFBC unless disabled */
   GLuint tex_b = make_target(&fbo_b);  /* second: linear via PVR_FORCE_LINEAR_RT=1 */
   static const GLfloat verts[] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
   GLuint p1 = glCreateProgram(), p2 = glCreateProgram();
   glAttachShader(p1, sh(GL_VERTEX_SHADER, vs_src)); glAttachShader(p1, sh(GL_FRAGMENT_SHADER, fs1_src));
   glBindAttribLocation(p1, 0, "pos"); glLinkProgram(p1);
   glAttachShader(p2, sh(GL_VERTEX_SHADER, vs_src)); glAttachShader(p2, sh(GL_FRAGMENT_SHADER, fs2_src));
   glBindAttribLocation(p2, 0, "pos"); glLinkProgram(p2);
   glViewport(0, 0, SIZE, SIZE);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
   glUseProgram(p1); glDrawArrays(GL_TRIANGLES, 0, 3);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
   glUseProgram(p2); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_a);
   glUniform1i(glGetUniformLocation(p2, "tex"), 0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   const char *mode = getenv("RTTEST_MODE");
   if (mode && !strcmp(mode, "rawclr")) {
      unsigned char *raw = calloc(SIZE * SIZE * 4, 1);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
      glClearColor(0.5f, 0.25f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glFinish();
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, raw);
      FILE *f = fopen("/tmp/raw.bin", "wb"); if (f) { fwrite(raw, 1, SIZE*SIZE*4, f); fclose(f); }
      printf("dumped raw clear\n");
      return 0;
   }
   if (mode && !strcmp(mode, "raw")) {
      /* pass 1's target as raw physical bytes (PVR_TWIDDLED_RAW_READBACK=1), plus the decoded copy
       * from B, for offline layout analysis. */
      unsigned char *raw = calloc(SIZE * SIZE * 4, 1);
      unsigned char *dec = calloc(SIZE * SIZE * 4, 1);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, dec);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, raw);
      FILE *f = fopen("/tmp/raw.bin", "wb"); if (f) { fwrite(raw, 1, SIZE*SIZE*4, f); fclose(f); }
      f = fopen("/tmp/decoded.bin", "wb"); if (f) { fwrite(dec, 1, SIZE*SIZE*4, f); fclose(f); }
      printf("dumped raw + decoded\n");
      return 0;
   }
   if (mode && !strcmp(mode, "clr")) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
      glClearColor(0.5f, 0.25f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
      glUseProgram(p2); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_a);
      glUniform1i(glGetUniformLocation(p2, "tex"), 0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      unsigned char *cp = malloc(SIZE * SIZE * 4);
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, cp);
      printf("clr px0=%02x %02x %02x %02x px_last=%02x %02x %02x %02x err=0x%x\n",
             cp[0], cp[1], cp[2], cp[3], cp[SIZE*SIZE*4-4], cp[SIZE*SIZE*4-3],
             cp[SIZE*SIZE*4-2], cp[SIZE*SIZE*4-1], glGetError());
      FILE *f = fopen("/tmp/clr.ppm", "wb");
      if (f) { fprintf(f, "P6\n%d %d\n255\n", SIZE, SIZE);
         for (int i = 0; i < SIZE*SIZE; i++) fwrite(&cp[i*4], 1, 3, f); fclose(f); }
      return 0;
   }
   unsigned char *px = malloc(SIZE * SIZE * 4);
   if (0 && mode && !strcmp(mode, "clear")) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
      glClearColor(0.5f, 0.25f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glFinish();
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
      printf("clear err=0x%x px0=%02x %02x %02x %02x\n", glGetError(), px[0], px[1], px[2], px[3]);
      return 0;
   }
   if (0 && mode && !strcmp(mode, "write")) {
      /* read pass 1's target directly (only possible on the linear control path) */
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
   } else if (0 && mode && !strcmp(mode, "sample")) {
      /* sampler path alone: upload a known pattern and sample it into B */
      unsigned char *src = malloc(SIZE * SIZE * 4);
      for (int y = 0; y < SIZE; y++) for (int x = 0; x < SIZE; x++) {
         unsigned char *q = &src[(y * SIZE + x) * 4];
         q[0] = x & 255; q[1] = y & 255; q[2] = 0; q[3] = 255;
      }
      GLuint stex; glGenTextures(1, &stex); glBindTexture(GL_TEXTURE_2D, stex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
      glUseProgram(p2); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, stex);
      glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
   } else {
      glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
   }
   int bad = 0, first = -1;
   for (int y = 0; y < SIZE; y++) for (int x = 0; x < SIZE; x++) {
      const unsigned char *p = &px[(y * SIZE + x) * 4];
      unsigned char er = x & 255, eg = y & 255;
      if (p[0] != er || p[1] != eg || p[2] != 0) { if (first < 0) first = y * SIZE + x; bad++; }
   }
   printf("bad=%d/%d first=%d\n", bad, SIZE * SIZE, first);
   if (bad) {
      int i = first * 4;
      printf("got %02x %02x %02x want %02x %02x 00\n", px[i], px[i+1], px[i+2],
             (first % SIZE) & 255, (first / SIZE) & 255);
      return 1;
   }
   printf("PASS\n");
   return 0;
}
