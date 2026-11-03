/* tqmip_probe: glGenerateMipmap results, level by level, for comparing two mip generation paths
 * (PVR_TQ_MIP unset vs set). Per size: draw a per-pixel pattern into level 0, generate, hash every
 * level read back through an FBO on that level (GL_OES_fbo_render_mipmap); then redraw level 0 with a
 * second pattern and regenerate (stale levels must not survive), and sample the chain minified from
 * a draw issued right after generation (that render must wait for the generation). Same output on
 * both paths = PASS; the diff is done by the caller (tools/verify/run-all.sh, group "transfer-queue
 * mip generation"). RGBX_ANY_RENDERER=1 allows a reference driver. */
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
static GLuint prog(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs)); glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "q"); glLinkProgram(p);
   return p;
}
static unsigned fnv(const unsigned char *p, size_t n)
{
   unsigned h = 2166136261u;
   for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
   return h;
}
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
   if (!strstr((const char *)glGetString(GL_EXTENSIONS), "GL_OES_fbo_render_mipmap"))
      { printf("RESULT: FAIL (no GL_OES_fbo_render_mipmap)\n"); return 1; }
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   const char *vs = "attribute vec2 q; varying vec2 v; void main(){ v = q * 0.5 + 0.5; gl_Position = vec4(q, 0.0, 1.0); }";
   /* Per-pixel distinct bytes: hashes of the integer pixel position, k selects the pattern. */
   GLuint fill = prog(vs, "precision highp float; uniform float k; void main(){"
                          " vec2 p = floor(gl_FragCoord.xy);"
                          " gl_FragColor = vec4(mod(p.x * 37.0 + p.y * 11.0 + k * 101.0, 256.0),"
                          " mod(p.x * 5.0 + p.y * 53.0 + k * 7.0, 256.0), mod(p.x * p.y + k * 29.0, 256.0),"
                          " mod(p.x + p.y * 3.0 + k, 256.0)) / 255.0; }");
   GLuint samp = prog(vs, "precision mediump float; varying vec2 v; uniform sampler2D t;"
                          " void main(){ gl_FragColor = texture2D(t, v); }");
   GLint uk = glGetUniformLocation(fill, "k");
   GLuint small, fsmall; glGenTextures(1, &small); glBindTexture(GL_TEXTURE_2D, small);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fsmall); glBindFramebuffer(GL_FRAMEBUFFER, fsmall);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, small, 0);
   static const int sz[][2] = { { 64, 64 }, { 256, 256 }, { 7, 5 }, { 100, 37 }, { 800, 600 }, { 1600, 1200 } };
   unsigned char *px = malloc(1600 * 1200 * 4);
   int errs = 0;
   for (unsigned z = 0; z < sizeof(sz) / sizeof(sz[0]); z++) {
      const int w = sz[z][0], h = sz[z][1];
      GLuint t, f; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenFramebuffers(1, &f);
      for (int pass = 0; pass < 2; pass++) {
         glBindFramebuffer(GL_FRAMEBUFFER, f);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
         glViewport(0, 0, w, h); glUseProgram(fill); glUniform1f(uk, (float)(pass + 1));
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glBindTexture(GL_TEXTURE_2D, t);
         glGenerateMipmap(GL_TEXTURE_2D);
         /* A draw right after generation that samples a small level (minified 32x32 target). */
         glBindFramebuffer(GL_FRAMEBUFFER, fsmall); glViewport(0, 0, 32, 32);
         glUseProgram(samp); glDrawArrays(GL_TRIANGLES, 0, 3);
         glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, px);
         printf("MIP %dx%d pass %d sampled32 %08x\n", w, h, pass, fnv(px, 32 * 32 * 4));
         glBindFramebuffer(GL_FRAMEBUFFER, f);
         for (int l = 0;; l++) {
            const int lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, l);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
               { printf("MIP %dx%d level %d: FBO incomplete\n", w, h, l); errs++; break; }
            glReadPixels(0, 0, lw, lh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("MIP %dx%d pass %d level %d (%dx%d) %08x px0 %02x%02x%02x%02x\n", w, h, pass, l, lw, lh,
                   fnv(px, (size_t)lw * lh * 4), px[0], px[1], px[2], px[3]);
            if (lw == 1 && lh == 1)
               break;
         }
      }
      if (glGetError() != GL_NO_ERROR) { printf("GL error at %dx%d\n", w, h); errs++; }
      glDeleteFramebuffers(1, &f); glDeleteTextures(1, &t);
   }
   printf("RESULT: %s\n", errs ? "FAIL" : "DONE");
   return errs != 0;
}
