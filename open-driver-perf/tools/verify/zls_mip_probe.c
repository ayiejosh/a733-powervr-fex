/* Gate probe for per-render state the draw path decides at flush time (review fixes on 0326/0367/0368/0370):
 *  A  depth written by a 3D pass is stored although the render's last draw disables depth
 *  B  a depth clear realized by a render that only tests depth reaches memory
 *  F  a depth clear in a render that never tests depth is handed to the next render
 *  C  glGenerateMipmap right after drawing into the texture (render still open) regenerates
 *  D  glGenerateMipmap after a clear-only render into the texture regenerates
 *  G  glGenerateMipmap while a depth-only framebuffer is bound writes the levels
 *  E  seven same-size renders in one batch each keep their own colour (RT data reuse)
 * Every case first puts known contents in memory through a readback, so a lost load, store or clear
 * shows on every build, not only where fresh memory happens to read as zero.
 * Prints one line per check and "RESULT: PASS" / "RESULT: FAIL". */
#include <stdio.h>
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

static GLuint prog(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "p"); glLinkProgram(p);
   return p;
}

static GLuint pcol, ptex;

/* Full-target triangle at depth z (NDC; window depth z/2 + 1/2) in a solid colour. */
static void quad(float z, float r, float g, float b)
{
   glUseProgram(pcol);
   glUniform1f(glGetUniformLocation(pcol, "z"), z);
   glUniform4f(glGetUniformLocation(pcol, "c"), r, g, b, 1.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

static GLuint fbo_tex(GLuint *tex, int w, GLuint depth_rb)
{
   GLuint f;
   glGenTextures(1, tex); glBindTexture(GL_TEXTURE_2D, *tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, w, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
   if (depth_rb)
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      printf("fbo incomplete\n");
   return f;
}

static int check(const char *name, int x, int y, unsigned char er, unsigned char eg, unsigned char eb)
{
   unsigned char px[4] = { 0 };
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const int ok = px[0] == er && px[1] == eg && px[2] == eb;
   printf("%s %s: got %d %d %d want %d %d %d err 0x%x\n", ok ? "PASS" : "FAIL", name, px[0], px[1],
          px[2], er, eg, eb, glGetError());
   return ok;
}

/* Stores depth z in fbo's 64x64 depth buffer on every build: the render ends in a readback while its
 * own depth state is still bound, so even a build that decides the store from the state bound at
 * flush time stores it. Leaves fbo bound with the depth test on, LESS, writes on. */
static void fill_depth(GLuint fbo, float z)
{
   unsigned char px[4];
   glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(0, 0, 64, 64);
   glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
   quad(z, 0, 0, 0);
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glDepthFunc(GL_LESS);
}

/* Ends any open render into tt (a draw into fb) and brings tt's mip chain up to date. */
static void sync_mips(GLuint fb, GLuint tt)
{
   glDisable(GL_DEPTH_TEST);
   glBindFramebuffer(GL_FRAMEBUFFER, fb); glViewport(0, 0, 64, 64); quad(0.0f, 0, 0, 0);
   glBindTexture(GL_TEXTURE_2D, tt); glGenerateMipmap(GL_TEXTURE_2D);
}

/* Draws tt into the 4x4 target fs: the derivative LOD is 4, so it samples generated level 4 (4x4). */
static void sample_mips(GLuint fs, GLuint tt)
{
   glDisable(GL_DEPTH_TEST);
   glBindFramebuffer(GL_FRAMEBUFFER, fs); glViewport(0, 0, 4, 4);
   glBindTexture(GL_TEXTURE_2D, tt);
   glUseProgram(ptex); glUniform1i(glGetUniformLocation(ptex, "t"), 0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
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
   printf("R=%s\nV=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   pcol = prog("attribute vec2 p; uniform float z; void main(){ gl_Position=vec4(p,z,1.0); }",
               "precision mediump float; uniform vec4 c; void main(){ gl_FragColor=c; }");
   ptex = prog("attribute vec2 p; varying vec2 uv; void main(){ uv=p*0.5+0.5; gl_Position=vec4(p,0.0,1.0); }",
               "precision mediump float; varying vec2 uv; uniform sampler2D t;"
               "void main(){ gl_FragColor=texture2D(t, uv); }");
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

   GLuint zrb; glGenRenderbuffers(1, &zrb); glBindRenderbuffer(GL_RENDERBUFFER, zrb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
   GLuint ta, tb, tt, ts;
   GLuint fa = fbo_tex(&ta, 64, zrb), fb = fbo_tex(&tb, 64, 0), fs = fbo_tex(&ts, 4, 0);
   GLuint ft = fbo_tex(&tt, 64, 0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
   GLuint fz; glGenFramebuffers(1, &fz); glBindFramebuffer(GL_FRAMEBUFFER, fz);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zrb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      printf("fbo incomplete (depth only)\n");
   glClearColor(0, 0, 0, 1); glClearDepthf(1.0f);
   int ok = 1;

   /* A: depth 0.95 stored; red at 0.25 with the depth test on, then a depth-off draw as the render's
    * last; binding fb ends the render. Then, depth writes off, blue at 0.125 and green at 0.375 over
    * it: the stored 0.25 passes blue only (blue); a lost store leaves 0.95 (green); depth that reads
    * 0 fails both (red). Not scissored halves: the probe predates the draw-scissor fix (0379) and
    * must not depend on it. */
   fill_depth(fa, 0.9f);
   quad(-0.5f, 1, 0, 0);
   glDisable(GL_DEPTH_TEST); quad(0.0f, 1, 0, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fb); quad(0.0f, 0, 0, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fa);
   glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
   quad(-0.75f, 0, 0, 1); quad(-0.25f, 0, 1, 0);
   glDepthMask(GL_TRUE);
   ok &= check("A depth stored past a depth-off last draw", 32, 32, 0, 0, 255);

   /* B: depth 0.25 stored; a render clears depth to 1.0 and only tests it (writes off); the next
    * render's blue at 0.5 must pass the cleared 1.0, not fail the old 0.25. */
   fill_depth(fa, -0.5f);
   glClear(GL_DEPTH_BUFFER_BIT);
   glDepthMask(GL_FALSE); quad(0.5f, 0, 1, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fb); quad(0.0f, 0, 0, 0);
   glDepthMask(GL_TRUE);
   glBindFramebuffer(GL_FRAMEBUFFER, fa); quad(0.0f, 0, 0, 1);
   ok &= check("B depth clear stored by a render that only tests depth", 32, 32, 0, 0, 255);

   /* F: depth 0.25 stored; a render clears depth to 1.0 and draws only with the depth test off; the
    * next render's blue at 0.5 must pass the cleared 1.0. */
   fill_depth(fa, -0.5f);
   glClear(GL_DEPTH_BUFFER_BIT);
   glDisable(GL_DEPTH_TEST); quad(0.0f, 1, 0, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fb); quad(0.0f, 0, 0, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fa);
   glEnable(GL_DEPTH_TEST); quad(0.0f, 0, 0, 1);
   ok &= check("F depth clear kept by a render that does not test depth", 32, 32, 0, 0, 255);

   /* C: chain red and in sync; green into level 0 and glGenerateMipmap while that render is still
    * open: level 4 must be green. */
   glDisable(GL_DEPTH_TEST);
   glBindFramebuffer(GL_FRAMEBUFFER, ft); glViewport(0, 0, 64, 64); quad(0.0f, 1, 0, 0);
   sync_mips(fb, tt);
   glBindFramebuffer(GL_FRAMEBUFFER, ft); quad(0.0f, 0, 1, 0);
   glBindTexture(GL_TEXTURE_2D, tt); glGenerateMipmap(GL_TEXTURE_2D);
   sample_mips(fs, tt);
   ok &= check("C mips after an open render into the texture", 1, 1, 0, 255, 0);

   /* D: chain in sync (green); a clear-only render makes level 0 blue and glFlush ends it;
    * glGenerateMipmap: level 4 blue. Not a draw into fb to end it: tt is still bound to unit 0, so
    * that draw realizes tt's clear itself, which bumps write_seq without the fix. */
   sync_mips(fb, tt);
   glBindFramebuffer(GL_FRAMEBUFFER, ft); glViewport(0, 0, 64, 64);
   glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glClearColor(0, 0, 0, 1);
   glFlush();
   glBindTexture(GL_TEXTURE_2D, tt); glGenerateMipmap(GL_TEXTURE_2D);
   sample_mips(fs, tt);
   ok &= check("D mips after a clear-only render", 1, 1, 0, 0, 255);

   /* G: chain red and in sync; green into level 0; a depth-only pass on fz ends that render; the
    * generation runs while fz is bound and must still write the levels: level 4 green. */
   glBindFramebuffer(GL_FRAMEBUFFER, ft); glViewport(0, 0, 64, 64); quad(0.0f, 1, 0, 0);
   sync_mips(fb, tt);
   glBindFramebuffer(GL_FRAMEBUFFER, ft); quad(0.0f, 0, 1, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, fz); glEnable(GL_DEPTH_TEST); quad(0.0f, 0, 0, 0);
   glBindTexture(GL_TEXTURE_2D, tt); glGenerateMipmap(GL_TEXTURE_2D);
   sample_mips(fs, tt);
   ok &= check("G mips generated while a depth-only framebuffer is bound", 1, 1, 0, 255, 0);

   /* E: seven 256x256 renders in one batch (nothing reads between them), each blending 64 layers of
    * its own colour: each texture must hold its colour. A size has two RT data sets that alternate,
    * so renders 3, 5 and 7 reuse the data of renders 1, 3 and 5 inside the batch. A pass here does
    * not prove the reuse is safe, only that this load did not break it. */
   GLuint te[7], fe[7];
   glDisable(GL_DEPTH_TEST);
   glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   for (int i = 0; i < 7; i++) {
      const int k = i + 1;
      fe[i] = fbo_tex(&te[i], 256, 0); glViewport(0, 0, 256, 256);
      for (int l = 0; l < 64; l++)
         quad(0.0f, k & 1, (k >> 1) & 1, (k >> 2) & 1);
   }
   glDisable(GL_BLEND);
   for (int i = 0; i < 7; i++) {
      const int k = i + 1;
      char name[48];
      snprintf(name, sizeof(name), "E batch render %d keeps its colour", k);
      glBindFramebuffer(GL_FRAMEBUFFER, fe[i]);
      ok &= check(name, 128, 128, (k & 1) * 255, ((k >> 1) & 1) * 255, ((k >> 2) & 1) * 255);
   }

   printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
   return ok ? 0 : 1;
}
