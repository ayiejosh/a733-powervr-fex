/* depthtest: value-level check of the native driver's depth path without CPU twiddling.
 * 1) fbo A (colour + depth, both twiddled when enabled) gets a depth-test scenario: a red quad at
 *    depth 0.75 then a green quad at depth 0.25 that must pass and win.
 * 2) fbo A's colour and depth are each sampled into a linear fbo B and read back, so the final
 *    colour and the stored depth value can be compared across native twiddled / linear / zink. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SIZE 256
static GLuint sh(GLenum t, const char *src) {
   GLuint s = glCreateShader(t); GLint ok = 0;
   glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(2); }
   return s;
}
static const GLfloat FULL[] = { -1, -1, 3, -1, -1, 3 };
static const char *vs_src =
   "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }";
static const char *vs_col_src =
   "attribute vec2 pos; uniform float z; void main() { gl_Position = vec4(pos, z, 1.0); }";
static const char *fs_src =
   "precision highp float; uniform vec4 colour; void main() { gl_FragColor = colour; }";
static const char *fs_tex_src =
   "precision highp float; uniform sampler2D tex; void main() { gl_FragColor = texture2D(tex, vec2(0.5, 0.5)); }";
static GLuint make_tex(int depth) {
   GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (depth)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SIZE, SIZE, 0, GL_DEPTH_COMPONENT,
                   GL_UNSIGNED_INT, NULL);
   else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   return t;
}
static void read_target(GLuint fbo, const char *what) {
   unsigned char px[4];
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glReadPixels(SIZE / 2, SIZE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   printf("%s=%02x %02x %02x %02x\n", what, px[0], px[1], px[2], px[3]);
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
      fprintf(stderr, "ctx failed\n"); return 2;
   }
   printf("R=%s\n", glGetString(GL_RENDERER));
   GLuint tex_a = make_tex(0), depth_a = make_tex(1), fbo_a, fbo_b, tex_b;
   glGenFramebuffers(1, &fbo_a); glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_a, 0);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_a, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo A incomplete\n"); return 2; }
   tex_b = make_tex(0);
   glGenFramebuffers(1, &fbo_b); glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_b, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo B incomplete\n"); return 2; }

   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof FULL, FULL, GL_STATIC_DRAW);
   GLuint pid = glCreateProgram();
   glAttachShader(pid, sh(GL_VERTEX_SHADER, vs_col_src)); glAttachShader(pid, sh(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(pid, 0, "pos"); glLinkProgram(pid);
   GLuint pt = glCreateProgram();
   glAttachShader(pt, sh(GL_VERTEX_SHADER, vs_src)); glAttachShader(pt, sh(GL_FRAGMENT_SHADER, fs_tex_src));
   glBindAttribLocation(pt, 0, "pos"); glLinkProgram(pt);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glViewport(0, 0, SIZE, SIZE);

   /* pass 1: red at depth 0.75, then green at depth 0.25: green must win */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
   glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
   glClearColor(0, 0, 0, 1); glClearDepthf(1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glUseProgram(pid);
   glUniform1f(glGetUniformLocation(pid, "z"), 0.5f);
   glUniform4f(glGetUniformLocation(pid, "colour"), 1, 0, 0, 1);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glUniform1f(glGetUniformLocation(pid, "z"), -0.5f);   /* closer: depth 0.25 */
   glUniform4f(glGetUniformLocation(pid, "colour"), 0, 1, 0, 1);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   /* pass 2: colour of A -> linear B, read */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
   glDisable(GL_DEPTH_TEST);
   glUseProgram(pt);
   glUniform1i(glGetUniformLocation(pt, "tex"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex_a);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   read_target(fbo_b, "colour");

   /* pass 3: depth of A -> linear B, read */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
   glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
   glBindTexture(GL_TEXTURE_2D, depth_a);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   read_target(fbo_b, "depth");
   printf("err=0x%x\n", glGetError());
   return 0;
}
