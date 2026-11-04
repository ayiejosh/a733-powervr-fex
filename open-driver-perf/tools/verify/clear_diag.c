/* Diagnostic, not a gate probe: why does a scissored glClear (st's clear quad: TRIANGLE_FAN at
 * z = 2 * clear_depth - 1, colour from a flat varying) leave nothing on the pvr native driver?
 *  D1 scissored clear, clear depth 1.0 (the failing S5 case)      D2 same, clear depth 0.5
 *  D3 same, clear depth 0.0 (z = -1)                               D4 colour-masked clear, no scissor
 *  D5-D8 a plain red draw at z = 1.0, 0.999, 0.0, -1.0              D9 scissored clear with no prior glClear
 * Prints one DIAG line per check. */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x);
   GLint ok = 0; glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
   if (!ok) { char l[512]; glGetShaderInfoLog(x, 512, NULL, l); fprintf(stderr, "compile %s\n", l); }
   return x;
}

static GLuint pz;

static void draw_z(float z, float r, float g, float b)
{
   glUseProgram(pz);
   glUniform1f(glGetUniformLocation(pz, "z"), z);
   glUniform4f(glGetUniformLocation(pz, "c"), r, g, b, 1.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void diag(const char *name, int x, int y, unsigned char er, unsigned char eg, unsigned char eb)
{
   unsigned char px[4] = { 0 };
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   printf("DIAG %s %s at %d,%d: got %d %d %d %d want %d %d %d err 0x%x\n",
          px[0] == er && px[1] == eg && px[2] == eb ? "ok " : "BAD", name, x, y,
          px[0], px[1], px[2], px[3], er, eg, eb, glGetError());
}

static void black(void)
{
   glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1); glClearDepthf(1.0f);
   glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
}

static void scissored_blue_clear(float depth)
{
   glClearDepthf(depth);
   glEnable(GL_SCISSOR_TEST); glScissor(5, 7, 32, 22);
   glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST); glClearDepthf(1.0f);
}

int main(void)
{
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
   printf("R=%s\nV=%s\n", rn ? rn : "(none)", vn ? vn : "(none)");
   if (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 || strstr(rn, "zink") != NULL ||
       strstr(vn, "Mesa") == NULL) {
      printf("DIAG BAD renderer is not the pvr native driver\n");
      return 1;
   }

   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER,
      "attribute vec2 p; uniform float z; void main(){ gl_Position=vec4(p,z,1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER,
      "precision mediump float; uniform vec4 c; void main(){ gl_FragColor=c; }"));
   glBindAttribLocation(p, 0, "p"); glLinkProgram(p); pz = p;
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

   GLuint tex, fbo;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glViewport(0, 0, 64, 64);

   black(); scissored_blue_clear(1.0f);
   diag("D1 scissored clear depth 1.0, inside", 20, 15, 0, 0, 255);
   diag("D1 scissored clear depth 1.0, outside", 60, 60, 0, 0, 0);
   black(); scissored_blue_clear(0.5f);
   diag("D2 scissored clear depth 0.5, inside", 20, 15, 0, 0, 255);
   black(); scissored_blue_clear(0.0f);
   diag("D3 scissored clear depth 0.0, inside", 20, 15, 0, 0, 255);
   black(); glColorMask(0, 0, 1, 1); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glColorMask(1, 1, 1, 1);
   diag("D4 colour-masked clear (blue+alpha only), no scissor", 20, 15, 0, 0, 255);
   black(); draw_z(1.0f, 1, 0, 0);
   diag("D5 draw at z=1.0", 20, 15, 255, 0, 0);
   black(); draw_z(0.999f, 1, 0, 0);
   diag("D6 draw at z=0.999", 20, 15, 255, 0, 0);
   black(); draw_z(0.0f, 1, 0, 0);
   diag("D7 draw at z=0.0", 20, 15, 255, 0, 0);
   black(); draw_z(-1.0f, 1, 0, 0);
   diag("D8 draw at z=-1.0", 20, 15, 255, 0, 0);
   black(); draw_z(0.0f, 0, 1, 0); scissored_blue_clear(1.0f);
   diag("D9 green draw then scissored clear depth 1.0, inside", 20, 15, 0, 0, 255);
   diag("D9 green draw then scissored clear depth 1.0, outside", 60, 60, 0, 255, 0);
   /* D10-D12: a vec4 attribute fed with fewer components; GL fills the missing ones from
    * (0, 0, 0, 1). The shader uses the attribute as gl_Position as it is. */
   {
      GLuint q = glCreateProgram();
      glAttachShader(q, sh(GL_VERTEX_SHADER, "attribute vec4 p; void main(){ gl_Position=p; }"));
      glAttachShader(q, sh(GL_FRAGMENT_SHADER,
         "precision mediump float; uniform vec4 c; void main(){ gl_FragColor=c; }"));
      glBindAttribLocation(q, 0, "p"); glLinkProgram(q); glUseProgram(q);
      glUniform4f(glGetUniformLocation(q, "c"), 1, 1, 0, 1);
      static const float v4[12] = { -1, -1, 0, 1,  3, -1, 0, 1,  -1, 3, 0, 1 };
      static const float v3[9] = { -1, -1, 0,  3, -1, 0,  -1, 3, 0 };
      static const float v2[6] = { -1, -1,  3, -1,  -1, 3 };
      GLuint b4, b3, b2;
      glGenBuffers(1, &b4); glBindBuffer(GL_ARRAY_BUFFER, b4); glBufferData(GL_ARRAY_BUFFER, sizeof(v4), v4, GL_STATIC_DRAW);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D10 vec4 attribute from 4 components (control)", 20, 15, 255, 255, 0);
      glGenBuffers(1, &b3); glBindBuffer(GL_ARRAY_BUFFER, b3); glBufferData(GL_ARRAY_BUFFER, sizeof(v3), v3, GL_STATIC_DRAW);
      black(); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D11 vec4 attribute from 3 components (w must be 1)", 20, 15, 255, 255, 0);
      glGenBuffers(1, &b2); glBindBuffer(GL_ARRAY_BUFFER, b2); glBufferData(GL_ARRAY_BUFFER, sizeof(v2), v2, GL_STATIC_DRAW);
      black(); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D12 vec4 attribute from 2 components (z 0, w 1)", 20, 15, 255, 255, 0);
      /* D13-D15: the clear quad is a 4-vertex TRIANGLE_FAN whose vertices st uploads through its
       * stream uploader. Separate the two: a fan from a buffer object, a fan from a client array
       * (st uploads it the same way), and triangles from a client array. */
      static const float fan[16] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
      static const float tri[12] = { -1, -1, 0, 1,  3, -1, 0, 1,  -1, 3, 0, 1 };
      GLuint bf; glGenBuffers(1, &bf); glBindBuffer(GL_ARRAY_BUFFER, bf); glBufferData(GL_ARRAY_BUFFER, sizeof(fan), fan, GL_STATIC_DRAW);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0); glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      diag("D13 TRIANGLE_FAN quad from a buffer object", 20, 15, 255, 255, 0);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, fan); glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      diag("D14 TRIANGLE_FAN quad from a client array", 20, 15, 255, 255, 0);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, tri); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D15 TRIANGLES from a client array", 20, 15, 255, 255, 0);
      /* D16-D19: the order matters. A 4-component draw with w = 2 first leaves w = 2 behind; a
       * 3-component draw then shows whether the missing w is filled with 1 (GL) or left over. */
      static const float w2[12] = { -2, -2, 0, 2,  6, -2, 0, 2,  -2, 6, 0, 2 };
      static const float t3[9] = { -1, -1, 0,  3, -1, 0,  -1, 3, 0 };
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, w2); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D16 4 components with w = 2 (same triangle after the divide)", 20, 15, 255, 255, 0);
      black(); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, t3); glDrawArrays(GL_TRIANGLES, 0, 3);
      diag("D17 3 components right after D16 (w must be 1, not 2)", 20, 15, 255, 255, 0);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, w2); glDrawArrays(GL_TRIANGLES, 0, 3);
      scissored_blue_clear(1.0f);
      diag("D18 scissored clear right after a w = 2 draw", 20, 15, 0, 0, 255);
      glUseProgram(q);
      black(); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, fan); glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      scissored_blue_clear(1.0f);
      diag("D19 scissored clear right after a w = 1 draw", 20, 15, 0, 0, 255);
   }
   glFinish();
   printf("DIAG-END\n");
   return 0;
}
