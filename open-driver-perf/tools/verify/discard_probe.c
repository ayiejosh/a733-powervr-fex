/* Gate probe (discard): a fragment shader that can discard must still write the pixels it keeps. Each
 * case fills a 16x16 RGBA8 target red, then draws green with a shader that discards per the case, and
 * reads (1,1) and (10,10). Before patch 0395 every shader containing discard wrote nothing: its
 * helper-pixel test read a sample mask from an fs_meta register this driver never fills.
 * RGBX_ANY_RENDERER=1 allows a reference driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x);
   GLint ok; glGetShaderiv(x, GL_COMPILE_STATUS, &ok); if (!ok) printf("compile failed: %s\n", s);
   return x;
}
static GLuint prog(const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "q"); glLinkProgram(p);
   GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) printf("link failed\n");
   return p;
}
static void rd(int x, int y, unsigned char px[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }

int main(void)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint a, b; eglInitialize(d, &a, &b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n = 0; EGLConfig cfg;
   EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
   eglChooseConfig(d, ca, &cfg, 1, &n);
   EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
   const char *rn = (const char *)glGetString(GL_RENDERER), *vn = (const char *)glGetString(GL_VERSION);
   printf("R=%s V=%s\n", rn ? rn : "(none)", vn ? vn : "(none)");
   if (!getenv("RGBX_ANY_RENDERER") && (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 ||
                                        strstr(rn, "zink") || strstr(vn, "Mesa") == NULL))
      { printf("RESULT: FAIL (wrong renderer)\n"); return 1; }
   int bad = 0;
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   GLuint red = prog("precision highp float; void main(){ gl_FragColor=vec4(1.0,0.0,0.0,1.0); }");
   const char *corner = "precision highp float; void main(){ if (gl_FragCoord.x > 4.0 || gl_FragCoord.y > 4.0) discard;"
                        " gl_FragColor=vec4(0.0,1.0,0.0,1.0); }";
   const char *never = "precision highp float; void main(){ if (gl_FragCoord.x < -1.0) discard;"
                       " gl_FragColor=vec4(0.0,1.0,0.0,1.0); }";
   const char *unif = "precision highp float; uniform float u; void main(){ if (u > 0.5) discard;"
                      " gl_FragColor=vec4(0.0,1.0,0.0,1.0); }";
   const char *plain = "precision highp float; void main(){ gl_FragColor=vec4(0.0,1.0,0.0,1.0); }";
   struct { const char *name, *fs; int fill_draw, flush, depth, want_c, want_f; } cs[] = {
      /* want_*: 1 = green, 0 = red, at corner (1,1) and far (10,10) */
      { "plain (control)", plain, 0, 0, 0, 1, 1 },
      { "corner, clear fill", corner, 0, 0, 0, 1, 0 },
      { "corner, draw fill", corner, 1, 0, 0, 1, 0 },
      { "corner, draw fill, flush", corner, 1, 1, 0, 1, 0 },
      { "never discards", never, 0, 0, 0, 1, 1 },
      { "uniform u=0", unif, 0, 0, 0, 1, 1 },
      { "corner, depth rb attached", corner, 0, 0, 1, 1, 0 },
   };
   for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
      GLuint t, f, rb = 0;
      glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
      if (cs[i].depth) {
         glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
         glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 16, 16);
         glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
      }
      glViewport(0, 0, 16, 16);
      if (cs[i].fill_draw) { glUseProgram(red); glDrawArrays(GL_TRIANGLES, 0, 3); }
      else { glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | (cs[i].depth ? GL_DEPTH_BUFFER_BIT : 0)); }
      if (cs[i].flush) glFlush();
      GLuint p = prog(cs[i].fs); glUseProgram(p);
      GLint ul = glGetUniformLocation(p, "u"); if (ul >= 0) glUniform1f(ul, 0.0f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      unsigned char c1[4], c2[4]; rd(1, 1, c1); rd(10, 10, c2);
      const int okc = cs[i].want_c ? (c1[0] == 0 && c1[1] == 255) : (c1[0] == 255 && c1[1] == 0);
      const int okf = cs[i].want_f ? (c2[0] == 0 && c2[1] == 255) : (c2[0] == 255 && c2[1] == 0);
      printf("DIAG %s discard %-26s 1,1 = %3d %3d %3d %3d  10,10 = %3d %3d %3d %3d  err=0x%x\n", okc && okf ? "OK " : "BAD",
             cs[i].name, c1[0], c1[1], c1[2], c1[3], c2[0], c2[1], c2[2], c2[3], glGetError());
      bad |= !(okc && okf);
   }
   printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
   return bad;
}
