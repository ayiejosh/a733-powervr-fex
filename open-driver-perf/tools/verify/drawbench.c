/* drawbench: fixed-work GLES2 draw benchmark that runs on surfaceless EGL, so the same binary can be
 * run on the native path (our Mesa + pvr_native) and on the vendor GL stack (Mesa zink + IMG Vulkan
 * ICD) without X/Wayland/DRM. Measures frames/second with a glFinish per frame. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W 800
#define H 600
#define DRAWS_MAX 2000
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static GLuint sh(GLenum t, const char *src) {
   GLuint s = glCreateShader(t); GLint ok = 0;
   glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(2); }
   return s;
}
static const char *vs_src =
   "attribute vec2 pos; attribute vec3 col; uniform float a; varying vec3 v_col;"
   " void main() { float c = cos(a), s = sin(a);"
   "  vec2 p = mat2(c, -s, s, c) * pos; v_col = col; gl_Position = vec4(p, 0.0, 1.0); }";
static const char *fs_src =
   "precision mediump float; varying vec3 v_col; uniform sampler2D tex; uniform int use_tex;"
   " void main() { vec4 c = vec4(v_col, 1.0);"
   "  if (use_tex != 0) c *= texture2D(tex, v_col.xy);"
   "  gl_FragColor = c; }";
int main(void) {
   const int draws = getenv("DRAWS") ? atoi(getenv("DRAWS")) : 200;
   const int frames_target = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 300;
   const int do_finish = getenv("FINISH") ? atoi(getenv("FINISH")) : 1;
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy;
   if (getenv("DB_X11"))
      dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   else
      dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "egl init failed\n"); return 2; }
   eglBindAPI(EGL_OPENGL_ES_API);
   static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLConfig cfg = EGL_NO_CONFIG_KHR;
   EGLSurface surf = EGL_NO_SURFACE;
   if (getenv("DB_X11")) {
      static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
      EGLint n = 0;
      if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no X11 config\n"); return 2; }
      static const EGLint pattr[] = { EGL_WIDTH, 512, EGL_HEIGHT, 512, EGL_NONE };
      surf = eglCreatePbufferSurface(dpy, cfg, pattr);
      if (surf == EGL_NO_SURFACE) { fprintf(stderr, "no pbuffer 0x%x\n", eglGetError()); return 2; }
   }
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "ctx failed 0x%x\n", eglGetError()); return 2; }
   printf("R=%s\nV=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   const char *mode = getenv("MODE") ? getenv("MODE") : "plain";
   GLuint fbo, ctex, dtex;
   glGenTextures(1, &ctex); glBindTexture(GL_TEXTURE_2D, ctex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenTextures(1, &dtex); glBindTexture(GL_TEXTURE_2D, dtex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, W, H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 2; }
   glViewport(0, 0, W, H);
   glEnable(GL_DEPTH_TEST);
   GLuint tex = 0;
   if (strcmp(mode, "tex") == 0) {
      unsigned char *px = malloc(256 * 256 * 4);
      for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
         unsigned char *q = &px[(y * 256 + x) * 4];
         q[0] = x; q[1] = y; q[2] = (x ^ y); q[3] = 255;
      }
      glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      free(px);
   } else if (strcmp(mode, "blend") == 0) {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   }
   /* 200 triangles spread over the screen; one VBO for positions+colours */
   static GLfloat verts[DRAWS_MAX * 3 * 5];
   for (int i = 0; i < draws; i++) {
      float cx = -0.95f + 1.9f * ((i % 20) / 19.0f), cy = -0.9f + 1.8f * ((i / 20) / ((draws + 19) / 20 - 0.0001f));
      for (int v = 0; v < 3; v++) {
         float *p = &verts[(i * 3 + v) * 5];
         p[0] = cx + 0.04f * (v == 1); p[1] = cy + 0.04f * (v == 2);
         p[2] = (i % 3) / 2.0f; p[3] = (v == 1) ? 1.0f : 0.0f; p[4] = 0.5f;
      }
   }
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)draws * 3 * 5 * sizeof(GLfloat), verts, GL_STATIC_DRAW);
   GLuint pid = glCreateProgram();
   glAttachShader(pid, sh(GL_VERTEX_SHADER, vs_src)); glAttachShader(pid, sh(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(pid, 0, "pos"); glBindAttribLocation(pid, 1, "col"); glLinkProgram(pid);
   glUseProgram(pid);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void *)0);
   glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, (void *)8);
   double t0 = now(); int frames = 0;
   while (frames < frames_target) {
      glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glUniform1f(glGetUniformLocation(pid, "a"), frames * 0.01f);
      glUniform1i(glGetUniformLocation(pid, "use_tex"), tex != 0);
      if (tex != 0) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }
      if (strcmp(mode, "state") == 0)
         glEnable(GL_DEPTH_TEST);
      glDrawArrays(GL_TRIANGLES, 0, draws * 3);
      if (strcmp(mode, "state") == 0)
         glDisable(GL_DEPTH_TEST);
      if (do_finish)
         glFinish();
      frames++;
      if (!do_finish)
         glFlush();
   }
   glFinish();
   double dt = now() - t0;
   printf("frames=%d time=%.3f fps=%.1f\n", frames, dt, frames / dt);
   return 0;
}
