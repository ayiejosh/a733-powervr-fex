/* Gate probe for draw scissor (glScissor applied to draws, not only to clears), 64x64 RGBA8 FBO:
 *  S1 a scissored full-target draw writes the box only; every box edge is checked on both sides
 *     (edges off the 16-pixel tile grid, so the region clip cannot be what passes it)
 *  S2 a draw with the scissor test off after a scissored one in the same render covers the target
 *  S3 four draws in one render, three boxes inside one 16x16 tile and one of them used again: only
 *     each draw's scissor index separates them
 *  S4 empty boxes (zero width, zero height, outside the target) draw nothing, and a later draw of
 *     the same render still lands (no lost or hung render)
 *  S5 a scissored glClear covers the box only (st clears it with a quad draw, scissor test off)
 *  S6 boxes with a negative origin and past the far edge (st clamps them to the target before the
 *     driver sees them; the case checks that the clamped box is honoured)
 *  S7 a colour-masked glClear keeps the masked channels (the same st quad path as S5)
 *  S8 70 distinct boxes with no flush between them, more than the 64-entry scissor table (entry 0
 *     is the whole target, so the render splits at box 63); every box keeps its pixel
 * The driver ignored the scissor for draws, so S1, S3, S4, S6 and S8 fail without the scissor fix;
 * S5 and S7 go through st's clear quad, which does not draw reliably (an open bug, round 231). zink applies
 * the scissor itself and passes everything, so a renderer other than the pvr native driver (a run
 * without MESA_LOADER_DRIVER_OVERRIDE=pvr) fails the run before any case.
 * Prints one line per check and "RESULT: PASS" / "RESULT: FAIL". */
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

static GLuint prog(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "p"); glLinkProgram(p);
   return p;
}

static GLuint pcol;

/* Full-target triangle in a solid colour. */
static void quad(float r, float g, float b)
{
   glUniform4f(glGetUniformLocation(pcol, "c"), r, g, b, 1.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

static int check(const char *name, int x, int y, unsigned char er, unsigned char eg, unsigned char eb)
{
   unsigned char px[4] = { 0 };
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const int ok = px[0] == er && px[1] == eg && px[2] == eb;
   printf("%s %s at %d,%d: got %d %d %d want %d %d %d err 0x%x\n", ok ? "PASS" : "FAIL", name, x, y,
          px[0], px[1], px[2], er, eg, eb, glGetError());
   return ok;
}

/* A case starts with the scissor test off (glClear is scissored too) and the target black. */
static void begin(void)
{
   glDisable(GL_SCISSOR_TEST);
   glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
}

/* Scissor test on with the box [x, x + w) x [y, y + h). */
static void box(int x, int y, int w, int h)
{
   glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h);
}

/* The box [5, 37) x [7, 29) and the pixels on both sides of each of its edges. */
static int check_edges(const char *c, unsigned char r, unsigned char g, unsigned char b)
{
   char n[64];
   int ok = 1;
   snprintf(n, sizeof(n), "%s left edge outside", c); ok &= check(n, 4, 15, 0, 0, 0);
   snprintf(n, sizeof(n), "%s left edge inside", c); ok &= check(n, 5, 15, r, g, b);
   snprintf(n, sizeof(n), "%s right edge inside", c); ok &= check(n, 36, 15, r, g, b);
   snprintf(n, sizeof(n), "%s right edge outside", c); ok &= check(n, 37, 15, 0, 0, 0);
   snprintf(n, sizeof(n), "%s bottom edge outside", c); ok &= check(n, 20, 6, 0, 0, 0);
   snprintf(n, sizeof(n), "%s bottom edge inside", c); ok &= check(n, 20, 7, r, g, b);
   snprintf(n, sizeof(n), "%s top edge inside", c); ok &= check(n, 20, 28, r, g, b);
   snprintf(n, sizeof(n), "%s top edge outside", c); ok &= check(n, 20, 29, 0, 0, 0);
   snprintf(n, sizeof(n), "%s first corner inside", c); ok &= check(n, 5, 7, r, g, b);
   snprintf(n, sizeof(n), "%s last corner inside", c); ok &= check(n, 36, 28, r, g, b);
   snprintf(n, sizeof(n), "%s far tile outside", c); ok &= check(n, 60, 60, 0, 0, 0);
   return ok;
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
   /* zink's renderer names the Vulkan device ("zink Vulkan ... (PowerVR ...)") and the vendor
    * driver is not Mesa: both apply the scissor and would pass without the fix. */
   if (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 || strstr(rn, "zink") != NULL ||
       strstr(vn, "Mesa") == NULL) {
      printf("FAIL S0 renderer is not the pvr native driver (MESA_LOADER_DRIVER_OVERRIDE=pvr unset?)\n");
      printf("RESULT: FAIL\n");
      return 1;
   }

   pcol = prog("attribute vec2 p; void main(){ gl_Position=vec4(p,0.0,1.0); }",
               "precision mediump float; uniform vec4 c; void main(){ gl_FragColor=c; }");
   glUseProgram(pcol);
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
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      printf("fbo incomplete\n");
   glViewport(0, 0, 64, 64);
   int ok = 1;

   /* S1: red through the box [5, 37) x [7, 29). */
   begin();
   box(5, 7, 32, 22); quad(1, 0, 0);
   ok &= check_edges("S1 scissored draw", 255, 0, 0);

   /* S2: in one render, red through the S1 box, then green with the scissor test off: all green. */
   begin();
   box(5, 7, 32, 22); quad(1, 0, 0);
   glDisable(GL_SCISSOR_TEST); quad(0, 1, 0);
   ok &= check("S2 unscissored draw after a scissored one, origin", 0, 0, 0, 255, 0);
   ok &= check("S2 unscissored draw after a scissored one, far corner", 63, 63, 0, 255, 0);
   ok &= check("S2 unscissored draw after a scissored one, left of box", 4, 15, 0, 255, 0);
   ok &= check("S2 unscissored draw after a scissored one, right of box", 37, 15, 0, 255, 0);
   ok &= check("S2 unscissored draw after a scissored one, in box", 20, 20, 0, 255, 0);

   /* S3: one render, all boxes in the first 16x16 tile: white everywhere, then red in A [1,7)x[1,7),
    * blue in B [9,15)x[1,7), green in C [1,7)x[9,15), and yellow in B again (an entry the render
    * already has, not its last one). */
   begin();
   quad(1, 1, 1);
   box(1, 1, 6, 6); quad(1, 0, 0);
   box(9, 1, 6, 6); quad(0, 0, 1);
   box(1, 9, 6, 6); quad(0, 1, 0);
   box(9, 1, 6, 6); quad(1, 1, 0);
   glDisable(GL_SCISSOR_TEST);
   ok &= check("S3 box A first pixel", 1, 1, 255, 0, 0);
   ok &= check("S3 box A last pixel", 6, 6, 255, 0, 0);
   ok &= check("S3 box B again first pixel", 9, 1, 255, 255, 0);
   ok &= check("S3 box B again last pixel", 14, 6, 255, 255, 0);
   ok &= check("S3 box C first pixel", 1, 9, 0, 255, 0);
   ok &= check("S3 box C last pixel", 6, 14, 0, 255, 0);
   ok &= check("S3 between A and B", 7, 3, 255, 255, 255);
   ok &= check("S3 between A and B", 8, 3, 255, 255, 255);
   ok &= check("S3 same tile outside the boxes", 12, 12, 255, 255, 255);
   ok &= check("S3 other tile", 40, 40, 255, 255, 255);

   /* S4: one render, red through a zero-width, a zero-height and an outside box, then green through
    * [40,48)x[40,48). */
   begin();
   box(10, 10, 0, 20); quad(1, 0, 0);
   box(10, 10, 20, 0); quad(1, 0, 0);
   box(80, 80, 10, 10); quad(1, 0, 0);
   box(40, 40, 8, 8); quad(0, 1, 0);
   glDisable(GL_SCISSOR_TEST);
   ok &= check("S4 zero-width box origin", 10, 10, 0, 0, 0);
   ok &= check("S4 zero-height box, on its row", 15, 10, 0, 0, 0);
   ok &= check("S4 empty boxes, between their edges", 15, 15, 0, 0, 0);
   ok &= check("S4 empty boxes, origin", 0, 0, 0, 0, 0);
   ok &= check("S4 empty boxes, far corner", 63, 63, 0, 0, 0);
   ok &= check("S4 draw after the empty ones, first pixel", 40, 40, 0, 255, 0);
   ok &= check("S4 draw after the empty ones, last pixel", 47, 47, 0, 255, 0);
   ok &= check("S4 draw after the empty ones, left of box", 39, 44, 0, 0, 0);
   ok &= check("S4 draw after the empty ones, right of box", 48, 44, 0, 0, 0);

   /* S5: blue glClear through the S1 box. */
   begin();
   box(5, 7, 32, 22);
   glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glClearColor(0, 0, 0, 1);
   glDisable(GL_SCISSOR_TEST);
   ok &= check_edges("S5 scissored clear", 0, 0, 255);

   /* S6: one render, red through [40,140)x[40,140) and blue through [-8,12)x[-8,12): clamped to
    * [40,64)x[40,64) and [0,12)x[0,12). */
   begin();
   box(40, 40, 100, 100); quad(1, 0, 0);
   box(-8, -8, 20, 20); quad(0, 0, 1);
   glDisable(GL_SCISSOR_TEST);
   ok &= check("S6 box past the far edge, first pixel", 40, 40, 255, 0, 0);
   ok &= check("S6 box past the far edge, last pixel", 63, 63, 255, 0, 0);
   ok &= check("S6 box past the far edge, left of box", 39, 50, 0, 0, 0);
   ok &= check("S6 box past the far edge, below box", 50, 39, 0, 0, 0);
   ok &= check("S6 negative origin box, origin", 0, 0, 0, 0, 255);
   ok &= check("S6 negative origin box, last pixel", 11, 11, 0, 0, 255);
   ok &= check("S6 negative origin box, right of box", 12, 5, 0, 0, 0);
   ok &= check("S6 negative origin box, above box", 5, 12, 0, 0, 0);
   ok &= check("S6 between the boxes", 25, 25, 0, 0, 0);

   /* S7: a colour-masked glClear (blue and alpha only) over a red draw, no scissor: st clears it
    * with the same quad draw as S5, so the red channel must survive and blue must land. */
   begin();
   quad(1, 0, 0);
   glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_TRUE);
   glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glClearColor(0, 0, 0, 1);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   ok &= check("S7 colour-masked clear over red, origin", 0, 0, 255, 0, 255);
   ok &= check("S7 colour-masked clear over red, far corner", 63, 63, 255, 0, 255);

   /* S8: one render, 70 distinct 1x1 boxes (more than the 64-entry table): the render must end and
    * reopen when the table is full, and every box keep its red pixel. */
   begin();
   for (int i = 0; i < 70; i++) {
      box(i % 64, i / 64, 1, 1);
      quad(1, 0, 0);
   }
   glDisable(GL_SCISSOR_TEST);
   ok &= check("S8 table overflow, first box", 0, 0, 255, 0, 0);
   ok &= check("S8 table overflow, first box of the second row", 0, 1, 255, 0, 0);
   ok &= check("S8 table overflow, last box of the first row", 63, 0, 255, 0, 0);
   ok &= check("S8 table overflow, last box", 5, 1, 255, 0, 0);
   ok &= check("S8 table overflow, after the last box", 6, 1, 0, 0, 0);
   ok &= check("S8 table overflow, outside every box", 10, 10, 0, 0, 0);

   printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
   return ok ? 0 : 1;
}
