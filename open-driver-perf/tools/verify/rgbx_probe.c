/* Gate probe for RGB (R8G8B8X8) textures, which the driver twiddles (TFBC stays off for them):
 *  X1 a 64x64 GL_RGB texture sampled texel-exact into an RGBA target (alpha must read 255)
 *  X2 an 800x600 GL_RGB texture (non-power-of-two, like glmark2's effect2d image), texel-exact
 *  X3 glTexSubImage2D into the X2 texture after it was sampled: the twiddled level is decoded,
 *     patched and re-encoded; the patch and the untouched texels must both be exact
 *  X4 a GL_RGB texture as a render target: draw into it, read it back with glReadPixels and by
 *     sampling it
 * Every check compares exact bytes. A renderer other than the pvr native driver fails the run,
 * unless RGBX_ANY_RENDERER=1 (to validate the probe itself on a reference driver such as zink).
 * Prints one line per check and "RESULT: PASS" / "RESULT: FAIL". */
#include <stdio.h>
#include <stdlib.h>
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

static unsigned char pr(int x, int y) { return (unsigned char)(x * 7 + y * 3); }
static unsigned char pg(int x, int y) { return (unsigned char)((x * 13) ^ (y * 5)); }
static unsigned char pb(int x, int y) { return (unsigned char)(x + y * 11); }

static unsigned char *pattern(int w, int h)
{
   unsigned char *d = malloc((size_t)w * h * 3);
   for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
         unsigned char *t = d + ((size_t)y * w + x) * 3;
         t[0] = pr(x, y); t[1] = pg(x, y); t[2] = pb(x, y);
      }
   return d;
}

static int check(const char *name, int x, int y, int er, int eg, int eb, int ea)
{
   unsigned char px[4] = { 0 };
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const int ok = px[0] == er && px[1] == eg && px[2] == eb && px[3] == ea;
   printf("%s %s at %d,%d: got %d %d %d %d want %d %d %d %d err 0x%x\n", ok ? "PASS" : "FAIL", name, x, y,
          px[0], px[1], px[2], px[3], er, eg, eb, ea, glGetError());
   return ok;
}

static GLuint psample;

/* Draw the whole target sampling tex texel-exact: pixel (x, y) reads texel (x, y). */
static void sample_into(GLuint fbo, GLuint tex, int w, int h)
{
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glViewport(0, 0, w, h);
   glUseProgram(psample);
   glUniform2f(glGetUniformLocation(psample, "sz"), (float)w, (float)h);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

static GLuint rgba_target(int w, int h, GLuint *tex)
{
   GLuint fbo;
   glGenTextures(1, tex); glBindTexture(GL_TEXTURE_2D, *tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
   return fbo;
}

static GLuint rgb_texture(int w, int h, const void *data)
{
   GLuint t;
   glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   return t;
}

static int check_pattern(const char *c, int w, int h, int skip_x0, int skip_y0, int skip_w, int skip_h)
{
   const int xs[] = { 0, 1, w / 3, w / 2, w - 2, w - 1 };
   const int ys[] = { 0, 1, h / 3, h / 2, h - 2, h - 1 };
   int ok = 1;
   char n[96];
   for (int i = 0; i < 6; i++)
      for (int j = 0; j < 6; j++) {
         const int x = xs[i], y = ys[j];
         if (x >= skip_x0 && x < skip_x0 + skip_w && y >= skip_y0 && y < skip_y0 + skip_h)
            continue;
         snprintf(n, sizeof(n), "%s texel", c);
         ok &= check(n, x, y, pr(x, y), pg(x, y), pb(x, y), 255);
      }
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
   if (!getenv("RGBX_ANY_RENDERER") &&
       (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 || strstr(rn, "zink") != NULL ||
        strstr(vn, "Mesa") == NULL)) {
      printf("FAIL X0 renderer is not the pvr native driver (MESA_LOADER_DRIVER_OVERRIDE=pvr unset?)\n");
      printf("RESULT: FAIL\n");
      return 1;
   }
   psample = prog("attribute vec2 p; void main(){ gl_Position=vec4(p,0.0,1.0); }",
                  "precision highp float; uniform sampler2D t; uniform vec2 sz;"
                  " void main(){ gl_FragColor=texture2D(t, gl_FragCoord.xy / sz); }");
   glUseProgram(psample);
   glUniform1i(glGetUniformLocation(psample, "t"), 0);
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   int ok = 1;

   /* X1: 64x64 */
   unsigned char *p64 = pattern(64, 64);
   GLuint t64 = rgb_texture(64, 64, p64);
   GLuint dst64; GLuint f64 = rgba_target(64, 64, &dst64);
   sample_into(f64, t64, 64, 64);
   ok &= check_pattern("X1 64x64 RGB", 64, 64, -1, -1, 0, 0);

   /* X2: 800x600 */
   unsigned char *pbig = pattern(800, 600);
   GLuint tbig = rgb_texture(800, 600, pbig);
   GLuint dstbig; GLuint fbig = rgba_target(800, 600, &dstbig);
   sample_into(fbig, tbig, 800, 600);
   ok &= check_pattern("X2 800x600 RGB", 800, 600, -1, -1, 0, 0);

   /* X3: patch [100,116)x[100,116) of the 800x600 texture after it was sampled */
   unsigned char patch[16 * 16 * 3];
   for (int i = 0; i < 16 * 16; i++) { patch[i * 3] = 10; patch[i * 3 + 1] = 200; patch[i * 3 + 2] = 30; }
   glBindTexture(GL_TEXTURE_2D, tbig);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 100, 100, 16, 16, GL_RGB, GL_UNSIGNED_BYTE, patch);
   sample_into(fbig, tbig, 800, 600);
   ok &= check("X3 patched texel first", 100, 100, 10, 200, 30, 255);
   ok &= check("X3 patched texel last", 115, 115, 10, 200, 30, 255);
   ok &= check_pattern("X3 untouched", 800, 600, 100, 100, 16, 16);
   ok &= check("X3 untouched next to patch", 116, 100, pr(116, 100), pg(116, 100), pb(116, 100), 255);
   ok &= check("X3 untouched below patch", 100, 99, pr(100, 99), pg(100, 99), pb(100, 99), 255);

   /* X4: an RGB texture as a render target: sample the 64x64 pattern into it, then read it back
    * directly and by sampling it into the RGBA target. */
   GLuint trt = rgb_texture(64, 64, NULL);
   GLuint frt; glGenFramebuffers(1, &frt); glBindFramebuffer(GL_FRAMEBUFFER, frt);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, trt, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("SKIP X4 GL_RGB texture is not colour-renderable here\n");
   } else {
      sample_into(frt, t64, 64, 64);
      ok &= check("X4 RGB target readback, texel", 10, 10, pr(10, 10), pg(10, 10), pb(10, 10), 255);
      ok &= check("X4 RGB target readback, pattern", 40, 50, pr(40, 50), pg(40, 50), pb(40, 50), 255);
      sample_into(f64, trt, 64, 64);
      ok &= check("X4 RGB target sampled, texel", 10, 10, pr(10, 10), pg(10, 10), pb(10, 10), 255);
      ok &= check("X4 RGB target sampled, pattern", 63, 0, pr(63, 0), pg(63, 0), pb(63, 0), 255);
   }
   printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
   return ok ? 0 : 1;
}
