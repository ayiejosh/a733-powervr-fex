/* Gate probe (texel-exact sampling): is texel-exact sampling off by a texel, and for which formats/sizes? For each format
 * (GL_RGBA, GL_RGB) and size, upload a pattern, sample it texel-exact (pixel (x,y) reads texel (x,y),
 * GL_NEAREST) into an RGBA target of the same size and report OK, or the (dx,dy) whose texel matches what
 * was read. Then odd-width targets and uploads (63x16 render readback, 63x40 partial glTexSubImage), a
 * texture sampled before its first upload, MRT attachment 1 read back or written right after its render,
 * glReadPixels from a GL_RGB render target, R8/RG8 render targets, a clear read back or patched with no
 * flush, MRT attachment 0 patched while bound, and an MRT framebuffer with a hole in its draw buffers.
 * RGBX_ANY_RENDERER=1 allows a reference driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define MIN2I(a, b) ((a) < (b) ? (a) : (b))
#define MAX2I(a, b) ((a) > (b) ? (a) : (b))
static unsigned char pr(int x, int y) { return (unsigned char)(x * 7 + y * 3 + 1); }
static unsigned char pg(int x, int y) { return (unsigned char)((x * 13) ^ (y * 5)); }
static unsigned char pb(int x, int y) { return (unsigned char)(x + y * 11); }

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x);
   return x;
}

static void readpx(int x, int y, unsigned char px[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }

/* Which texel offset (dx, dy) in [-3, 3]^2 explains the pixel read at (x, y)? */
static int find_off(int x, int y, const unsigned char px[3], int *dx, int *dy)
{
   for (int j = -3; j <= 3; j++)
      for (int i = -3; i <= 3; i++)
         if (px[0] == pr(x + i, y + j) && px[1] == pg(x + i, y + j) && px[2] == pb(x + i, y + j)) {
            *dx = i; *dy = j; return 1;
         }
   return 0;
}

int main(void)
{
   setvbuf(stdout, NULL, _IOLBF, 0); /* keep every line if a case crashes the driver */
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
   if (!getenv("RGBX_ANY_RENDERER") && (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 ||
                                        strstr(rn, "zink") || strstr(vn, "Mesa") == NULL))
      { printf("DIAG wrong renderer\n"); return 1; }
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision highp float; uniform sampler2D t; uniform vec2 sz;"
                        " void main(){ gl_FragColor=texture2D(t, gl_FragCoord.xy / sz); }"));
   glBindAttribLocation(p, 0, "q"); glLinkProgram(p); glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "t"), 0);
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   const int sizes[][2] = { {8, 8}, {64, 64}, {62, 62}, {63, 63}, {65, 65}, {66, 66}, {127, 127}, {7, 7}, {1, 8}, {63, 40}, {100, 60}, {800, 600} };
   const GLenum fmts[] = { GL_RGBA, GL_RGB };
   for (unsigned f = 0; f < 2; f++)
      for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
         const int w = sizes[s][0], h = sizes[s][1], cpp = fmts[f] == GL_RGBA ? 4 : 3;
         unsigned char *data = malloc((size_t)w * h * cpp);
         for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
               unsigned char *t = data + ((size_t)y * w + x) * cpp;
               t[0] = pr(x, y); t[1] = pg(x, y); t[2] = pb(x, y); if (cpp == 4) t[3] = 255;
            }
         GLuint tex, dst, fbo;
         glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
         glTexImage2D(GL_TEXTURE_2D, 0, fmts[f], w, h, 0, fmts[f], GL_UNSIGNED_BYTE, data);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
         glGenTextures(1, &dst); glBindTexture(GL_TEXTURE_2D, dst);
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
         glViewport(0, 0, w, h);
         glUniform2f(glGetUniformLocation(p, "sz"), (float)w, (float)h);
         glBindTexture(GL_TEXTURE_2D, tex);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         /* Check points clamped into the texture (a 1-wide texture has only x = 0). */
         const int pts[][2] = { {0, 0}, {MIN2I(2, w - 1), MIN2I(2, h - 1)}, {w / 2, h / 2}, {MAX2I(w - 3, 0), MAX2I(h - 3, 0)} };
         int ok = 1; char detail[256] = "";
         for (int i = 0; i < 4; i++) {
            unsigned char px[4]; readpx(pts[i][0], pts[i][1], px);
            if (px[0] != pr(pts[i][0], pts[i][1]) || px[1] != pg(pts[i][0], pts[i][1]) || px[2] != pb(pts[i][0], pts[i][1])) {
               int dx = 99, dy = 99; find_off(pts[i][0], pts[i][1], px, &dx, &dy);
               ok = 0;
               snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), " at(%d,%d) off(%d,%d)", pts[i][0], pts[i][1], dx, dy);
            }
         }
         printf("DIAG %s %s %dx%d%s\n", ok ? "OK " : "BAD", fmts[f] == GL_RGBA ? "RGBA" : "RGB ", w, h, detail);
         glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex); glDeleteTextures(1, &dst); free(data);
      }
   /* A 63-wide target: render floor(gl_FragCoord) into it and read it back (no upload involved). */
   {
      GLuint p2 = glCreateProgram();
      glAttachShader(p2, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
      glAttachShader(p2, sh(GL_FRAGMENT_SHADER, "precision highp float; void main(){ gl_FragColor=vec4(floor(gl_FragCoord.xy) / 255.0, 0.5, 1.0); }"));
      glBindAttribLocation(p2, 0, "q"); glLinkProgram(p2); glUseProgram(p2);
      GLuint t63, f63; glGenTextures(1, &t63); glBindTexture(GL_TEXTURE_2D, t63);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 63, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &f63); glBindFramebuffer(GL_FRAMEBUFFER, f63);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t63, 0);
      glViewport(0, 0, 63, 16); glDrawArrays(GL_TRIANGLES, 0, 3);
      int ok63 = 1; const int q[][2] = { {0, 0}, {62, 0}, {5, 9}, {40, 15}, {62, 15} };
      for (int i = 0; i < 5; i++) { unsigned char o[4]; readpx(q[i][0], q[i][1], o);
         if (o[0] != q[i][0] || o[1] != q[i][1]) { ok63 = 0; printf("DIAG     63x16 readback at %d,%d got %d %d\n", q[i][0], q[i][1], o[0], o[1]); } }
      printf("DIAG %s 63x16 render-target readback\n", ok63 ? "OK " : "BAD");
      glUseProgram(p);
   }
   /* A 63-wide texture: partial glTexSubImage, then sample a region the update did not touch. */
   {
      const int w = 63, h = 40; unsigned char *dd = malloc((size_t)w * h * 4);
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { unsigned char *tt = dd + ((size_t)y * w + x) * 4;
         tt[0] = pr(x, y); tt[1] = pg(x, y); tt[2] = pb(x, y); tt[3] = 255; }
      GLuint ts, dsx, fs2; glGenTextures(1, &ts); glBindTexture(GL_TEXTURE_2D, ts);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, dd);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      unsigned char patch[4 * 4 * 4]; memset(patch, 200, sizeof(patch));
      glTexSubImage2D(GL_TEXTURE_2D, 0, 10, 10, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, patch);
      glGenTextures(1, &dsx); glBindTexture(GL_TEXTURE_2D, dsx);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fs2); glBindFramebuffer(GL_FRAMEBUFFER, fs2);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dsx, 0);
      glViewport(0, 0, w, h); glUniform2f(glGetUniformLocation(p, "sz"), (float)w, (float)h);
      glBindTexture(GL_TEXTURE_2D, ts); glDrawArrays(GL_TRIANGLES, 0, 3);
      int oks = 1; const int q[][2] = { {0, 0}, {30, 30}, {62, 39}, {14, 10}, {9, 13} };
      for (int i = 0; i < 5; i++) { unsigned char o[4]; readpx(q[i][0], q[i][1], o);
         if (o[0] != pr(q[i][0], q[i][1]) || o[1] != pg(q[i][0], q[i][1]) || o[2] != pb(q[i][0], q[i][1])) {
            oks = 0; printf("DIAG     63x40 subimage untouched at %d,%d got %d %d %d\n", q[i][0], q[i][1], o[0], o[1], o[2]); } }
      unsigned char o[4]; readpx(11, 11, o); if (o[0] != 200) { oks = 0; printf("DIAG     63x40 patch at 11,11 got %d\n", o[0]); }
      printf("DIAG %s 63x40 partial glTexSubImage\n", oks ? "OK " : "BAD");
      free(dd);
   }
   /* Sampled before its first upload (undefined content, still a compressed descriptor), then filled
    * with glTexSubImage2D and sampled again with no flush in between: the second draw must read the
    * upload (the first CPU write drops the compression; descriptors read the flag at each draw). */
   {
      const int w = 64, h = 64; unsigned char *dd = malloc((size_t)w * h * 4);
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { unsigned char *tt = dd + ((size_t)y * w + x) * 4;
         tt[0] = pr(x, y); tt[1] = pg(x, y); tt[2] = pb(x, y); tt[3] = 255; }
      GLuint tl, dl, fl; glGenTextures(1, &tl); glBindTexture(GL_TEXTURE_2D, tl);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenTextures(1, &dl); glBindTexture(GL_TEXTURE_2D, dl);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fl); glBindFramebuffer(GL_FRAMEBUFFER, fl);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dl, 0);
      glViewport(0, 0, w, h); glUniform2f(glGetUniformLocation(p, "sz"), (float)w, (float)h);
      glBindTexture(GL_TEXTURE_2D, tl); glDrawArrays(GL_TRIANGLES, 0, 3);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dd);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      int okl = 1; const int q[][2] = { {0, 0}, {31, 17}, {63, 63} };
      for (int i = 0; i < 3; i++) { unsigned char o[4]; readpx(q[i][0], q[i][1], o);
         if (o[0] != pr(q[i][0], q[i][1]) || o[1] != pg(q[i][0], q[i][1]) || o[2] != pb(q[i][0], q[i][1])) {
            okl = 0; printf("DIAG     64x64 late upload at %d,%d got %d %d %d\n", q[i][0], q[i][1], o[0], o[1], o[2]); } }
      printf("DIAG %s 64x64 sampled before its first upload\n", okl ? "OK " : "BAD");
      free(dd);
   }
   /* MRT: a render writes attachment 1 of a 4x4 framebuffer (4x4 targets are linear), then the
    * attachment is read back through another framebuffer with no flush in between: the CPU map
    * must wait for the render that writes it. */
   {
      const char *ext = (const char *)glGetString(GL_EXTENSIONS);
      void (*draw_buffers)(GLsizei, const GLenum *) = (void *)eglGetProcAddress("glDrawBuffersEXT");
      if (ext == NULL || strstr(ext, "GL_EXT_draw_buffers") == NULL || draw_buffers == NULL) {
         printf("DIAG BAD 4x4 MRT attachment readback: GL_EXT_draw_buffers missing\n");
      } else {
         GLuint pm = glCreateProgram();
         glAttachShader(pm, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
         glAttachShader(pm, sh(GL_FRAGMENT_SHADER, "#extension GL_EXT_draw_buffers : require\nprecision highp float;"
                               " void main(){ gl_FragData[0]=vec4(1.0,0.0,0.0,1.0); gl_FragData[1]=vec4(0.0,1.0,0.0,1.0); }"));
         glBindAttribLocation(pm, 0, "q"); glLinkProgram(pm); glUseProgram(pm);
         GLuint tm[2], fm, fr;
         glGenTextures(2, tm);
         for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, tm[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         }
         glGenFramebuffers(1, &fm); glBindFramebuffer(GL_FRAMEBUFFER, fm);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tm[0], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, tm[1], 0);
         const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1 };
         draw_buffers(2, bufs);
         glViewport(0, 0, 4, 4); glDrawArrays(GL_TRIANGLES, 0, 3);
         glGenFramebuffers(1, &fr); glBindFramebuffer(GL_FRAMEBUFFER, fr);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tm[1], 0);
         unsigned char o[4]; readpx(2, 2, o);
         printf("DIAG %s 4x4 MRT attachment 1 read through another framebuffer got %d %d %d %d want 0 255 0 255\n",
                o[0] == 0 && o[1] == 255 && o[2] == 0 && o[3] == 255 ? "OK " : "BAD", o[0], o[1], o[2], o[3]);
         /* Two more fresh renders into the same framebuffer: attachment 1 read with the MRT framebuffer
          * still bound (NV_read_buffer), and a glTexSubImage into attachment 1 right after the draw. Both
          * must be ordered after the render that writes attachment 1. */
         void (*read_buffer)(GLenum) = (void *)eglGetProcAddress("glReadBufferNV");
         GLuint tb[4];
         glGenTextures(4, tb);
         for (int i = 0; i < 4; i++) {
            glBindTexture(GL_TEXTURE_2D, tb[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         }
         glBindFramebuffer(GL_FRAMEBUFFER, fm);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tb[0], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, tb[1], 0);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         if (strstr(ext, "GL_NV_read_buffer") == NULL || read_buffer == NULL) {
            printf("DIAG BAD 4x4 MRT attachment 1 read in place: GL_NV_read_buffer missing\n");
         } else {
            read_buffer(GL_COLOR_ATTACHMENT0 + 1); readpx(2, 2, o);
            printf("DIAG %s 4x4 MRT attachment 1 read with the MRT framebuffer still bound got %d %d %d %d want 0 255 0 255\n",
                   o[0] == 0 && o[1] == 255 && o[2] == 0 && o[3] == 255 ? "OK " : "BAD", o[0], o[1], o[2], o[3]);
            read_buffer(GL_COLOR_ATTACHMENT0);
         }
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tb[2], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, tb[3], 0);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         static const unsigned char blue[4] = { 0, 0, 255, 255 };
         glBindTexture(GL_TEXTURE_2D, tb[3]);
         glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
         glBindFramebuffer(GL_FRAMEBUFFER, fr);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tb[3], 0);
         unsigned char a1[4], a2[4]; readpx(1, 1, a1); readpx(2, 2, a2);
         printf("DIAG %s 4x4 MRT attachment 1 written by glTexSubImage after the draw got %d %d %d %d at 1,1 (want 0 0 255 255) and %d %d %d %d at 2,2 (want 0 255 0 255)\n",
                a1[0] == 0 && a1[1] == 0 && a1[2] == 255 && a1[3] == 255 && a2[0] == 0 && a2[1] == 255 && a2[2] == 0 && a2[3] == 255 ? "OK " : "BAD",
                a1[0], a1[1], a1[2], a1[3], a2[0], a2[1], a2[2], a2[3]);
         glUseProgram(p);
      }
   }
   /* RGB render target readback */
   GLuint rt, f2; glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &f2); glBindFramebuffer(GL_FRAMEBUFFER, f2);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
   printf("DIAG rgb-rt status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
   glViewport(0, 0, 64, 64); glClearColor(0.2f, 0.4f, 0.6f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
   unsigned char px[4]; readpx(10, 10, px);
   printf("DIAG %s rgb-rt cleared readback got %d %d %d %d want 51 102 153 255\n",
          px[0] == 51 && px[1] == 102 && px[2] == 153 && px[3] == 255 ? "OK " : "BAD", px[0], px[1], px[2], px[3]);
   /* R8 and RG8 render targets (EXT_texture_rg): a gradient drawn into 64x64 targets reads back exact (a
    * uniform clear hides a wrong pixel size in the PBE pack mode). */
   {
      GLuint pg = glCreateProgram();
      glAttachShader(pg, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
      glAttachShader(pg, sh(GL_FRAGMENT_SHADER, "precision highp float; void main(){ gl_FragColor=vec4(floor(gl_FragCoord.xy) / 255.0, 0.5, 1.0); }"));
      glBindAttribLocation(pg, 0, "q"); glLinkProgram(pg); glUseProgram(pg);
      static unsigned char gb[64 * 64 * 2];
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      for (int rg = 0; rg < 2; rg++) {
         GLuint tg, fg;
         glGenTextures(1, &tg); glBindTexture(GL_TEXTURE_2D, tg);
         glTexImage2D(GL_TEXTURE_2D, 0, rg ? GL_RG8_EXT : GL_R8_EXT, 64, 64, 0, rg ? GL_RG_EXT : GL_RED_EXT, GL_UNSIGNED_BYTE, NULL);
         glGenFramebuffers(1, &fg); glBindFramebuffer(GL_FRAMEBUFFER, fg);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tg, 0);
         glViewport(0, 0, 64, 64); glDrawArrays(GL_TRIANGLES, 0, 3);
         glReadPixels(0, 0, 64, 64, rg ? GL_RG_EXT : GL_RED_EXT, GL_UNSIGNED_BYTE, gb);
         int wrong = 0;
         for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
               const unsigned char *t = gb + (y * 64 + x) * (rg ? 2 : 1);
               if (t[0] != x || (rg && t[1] != y)) wrong++;
            }
         printf("DIAG %s 64x64 %s render target gradient readback (%d of 4096 wrong)\n", wrong ? "BAD" : "OK ", rg ? "RG8" : "R8", wrong);
      }
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glUseProgram(p);
   }
   /* A clear read back with no flush in between, on a 4x4 RGBA8 target (linear) and a 64x64 R8 target, and
    * a clear followed by a one-texel glTexSubImage2D: a CPU map must apply the clear first. */
   {
      for (int r8 = 0; r8 < 2; r8++) {
         const int n = r8 ? 64 : 4;
         GLuint tc, fc;
         glGenTextures(1, &tc); glBindTexture(GL_TEXTURE_2D, tc);
         glTexImage2D(GL_TEXTURE_2D, 0, r8 ? GL_R8_EXT : GL_RGBA, n, n, 0, r8 ? GL_RED_EXT : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         glGenFramebuffers(1, &fc); glBindFramebuffer(GL_FRAMEBUFFER, fc);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tc, 0);
         glViewport(0, 0, n, n); glClearColor(0.2f, 0.4f, 0.6f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
         unsigned char c[4] = { 0 };
         glReadPixels(1, 1, 1, 1, r8 ? GL_RED_EXT : GL_RGBA, GL_UNSIGNED_BYTE, c);
         printf("DIAG %s %dx%d %s clear read back with no flush got %d (want 51)\n", c[0] == 51 ? "OK " : "BAD", n, n, r8 ? "R8" : "RGBA8", c[0]);
      }
      GLuint tw, fw;
      glGenTextures(1, &tw); glBindTexture(GL_TEXTURE_2D, tw);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &fw); glBindFramebuffer(GL_FRAMEBUFFER, fw);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tw, 0);
      glViewport(0, 0, 4, 4); glClearColor(0.2f, 0.4f, 0.6f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
      static const unsigned char texel[4] = { 0, 0, 255, 255 };
      glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel);
      unsigned char w1[4], w2[4]; readpx(1, 1, w1); readpx(2, 2, w2);
      printf("DIAG %s 4x4 clear then glTexSubImage got %d %d %d %d at 1,1 (want 0 0 255 255) and %d %d %d %d at 2,2 (want 51 102 153 255)\n",
             w1[0] == 0 && w1[1] == 0 && w1[2] == 255 && w1[3] == 255 && w2[0] == 51 && w2[1] == 102 && w2[2] == 153 && w2[3] == 255 ? "OK " : "BAD",
             w1[0], w1[1], w1[2], w1[3], w2[0], w2[1], w2[2], w2[3]);
   }
   /* MRT 16x16 (twiddled): both attachments drawn, then a partial glTexSubImage2D into attachment 0 while
    * the MRT framebuffer is still bound (the map's encode is a render into attachment 0, which must not be
    * built as the framebuffer's MRT render): attachment 1 must keep the draw. */
   {
      const char *ext = (const char *)glGetString(GL_EXTENSIONS);
      void (*draw_buffers)(GLsizei, const GLenum *) = (void *)eglGetProcAddress("glDrawBuffersEXT");
      if (ext == NULL || strstr(ext, "GL_EXT_draw_buffers") == NULL || draw_buffers == NULL) {
         printf("DIAG BAD 16x16 MRT attachment 0 patch: GL_EXT_draw_buffers missing\n");
      } else {
         GLuint ps = glCreateProgram();
         glAttachShader(ps, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
         glAttachShader(ps, sh(GL_FRAGMENT_SHADER, "#extension GL_EXT_draw_buffers : require\nprecision highp float;"
                               " void main(){ gl_FragData[0]=vec4(1.0,0.0,0.0,1.0); gl_FragData[1]=vec4(0.0,1.0,0.0,1.0); }"));
         glBindAttribLocation(ps, 0, "q"); glLinkProgram(ps); glUseProgram(ps);
         GLuint ts[2], fs, fs1;
         glGenTextures(2, ts);
         for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, ts[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         }
         glGenFramebuffers(1, &fs); glBindFramebuffer(GL_FRAMEBUFFER, fs);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ts[0], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, ts[1], 0);
         const GLenum sbufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1 };
         draw_buffers(2, sbufs);
         glViewport(0, 0, 16, 16); glDrawArrays(GL_TRIANGLES, 0, 3);
         static const unsigned char patch0[4 * 4 * 4] = { 0 };
         glBindTexture(GL_TEXTURE_2D, ts[0]);
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, patch0);
         glGenFramebuffers(1, &fs1); glBindFramebuffer(GL_FRAMEBUFFER, fs1);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ts[1], 0);
         unsigned char s1[4], s2[4]; readpx(10, 10, s1); readpx(1, 1, s2);
         printf("DIAG %s 16x16 MRT attachment 0 patched while bound got %d %d %d %d at 10,10 and %d %d %d %d at 1,1 on attachment 1 (want 0 255 0 255 at both)\n",
                s1[0] == 0 && s1[1] == 255 && s1[2] == 0 && s1[3] == 255 && s2[0] == 0 && s2[1] == 255 && s2[2] == 0 && s2[3] == 255 ? "OK " : "BAD",
                s1[0], s1[1], s1[2], s1[3], s2[0], s2[1], s2[2], s2[3]);
         glUseProgram(p);
      }
   }
   /* MRT 16x16: both attachments drawn, then a scissored glClear (st draws it as a quad that writes
    * gl_FragColor, which goes to every bound colour buffer): attachment 1 must be cleared inside the
    * scissor and keep the draw outside it. */
   {
      const char *ext = (const char *)glGetString(GL_EXTENSIONS);
      void (*draw_buffers)(GLsizei, const GLenum *) = (void *)eglGetProcAddress("glDrawBuffersEXT");
      if (ext == NULL || strstr(ext, "GL_EXT_draw_buffers") == NULL || draw_buffers == NULL) {
         printf("DIAG BAD 16x16 MRT scissored clear: GL_EXT_draw_buffers missing\n");
      } else {
         GLuint pc = glCreateProgram();
         glAttachShader(pc, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
         glAttachShader(pc, sh(GL_FRAGMENT_SHADER, "#extension GL_EXT_draw_buffers : require\nprecision highp float;"
                               " void main(){ gl_FragData[0]=vec4(1.0,0.0,0.0,1.0); gl_FragData[1]=vec4(0.0,1.0,0.0,1.0); }"));
         glBindAttribLocation(pc, 0, "q"); glLinkProgram(pc); glUseProgram(pc);
         GLuint tq[2], fq, fq1;
         glGenTextures(2, tq);
         for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, tq[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         }
         glGenFramebuffers(1, &fq); glBindFramebuffer(GL_FRAMEBUFFER, fq);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tq[0], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, tq[1], 0);
         const GLenum qbufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1 };
         draw_buffers(2, qbufs);
         glViewport(0, 0, 16, 16); glDrawArrays(GL_TRIANGLES, 0, 3);
         glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 4, 4);
         glClearColor(0.0f, 0.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
         glDisable(GL_SCISSOR_TEST);
         glGenFramebuffers(1, &fq1); glBindFramebuffer(GL_FRAMEBUFFER, fq1);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tq[1], 0);
         unsigned char q1[4], q2[4]; readpx(1, 1, q1); readpx(10, 10, q2);
         printf("DIAG %s 16x16 MRT scissored clear got %d %d %d %d at 1,1 (want 0 0 255 255) and %d %d %d %d at 10,10 (want 0 255 0 255) on attachment 1\n",
                q1[0] == 0 && q1[1] == 0 && q1[2] == 255 && q1[3] == 255 && q2[0] == 0 && q2[1] == 255 && q2[2] == 0 && q2[3] == 255 ? "OK " : "BAD",
                q1[0], q1[1], q1[2], q1[3], q2[0], q2[1], q2[2], q2[3]);
         glUseProgram(p);
      }
   }
   /* Last, because it crashed older builds: MRT with a hole in the draw buffers ({0, NONE, 2}). Attachment 0
    * must be drawn (attachments past a hole are not supported yet). */
   {
      const char *ext = (const char *)glGetString(GL_EXTENSIONS);
      void (*draw_buffers)(GLsizei, const GLenum *) = (void *)eglGetProcAddress("glDrawBuffersEXT");
      if (ext == NULL || strstr(ext, "GL_EXT_draw_buffers") == NULL || draw_buffers == NULL) {
         printf("DIAG BAD 4x4 MRT with a hole: GL_EXT_draw_buffers missing\n");
      } else {
         GLuint ph = glCreateProgram();
         glAttachShader(ph, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
         glAttachShader(ph, sh(GL_FRAGMENT_SHADER, "#extension GL_EXT_draw_buffers : require\nprecision highp float;"
                               " void main(){ gl_FragData[0]=vec4(1.0,0.0,0.0,1.0); gl_FragData[2]=vec4(0.0,1.0,0.0,1.0); }"));
         glBindAttribLocation(ph, 0, "q"); glLinkProgram(ph); glUseProgram(ph);
         GLuint th[2], fh;
         glGenTextures(2, th);
         for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, th[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
         }
         glGenFramebuffers(1, &fh); glBindFramebuffer(GL_FRAMEBUFFER, fh);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, th[0], 0);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_TEXTURE_2D, th[1], 0);
         const GLenum hbufs[3] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT0 + 2 };
         draw_buffers(3, hbufs);
         glViewport(0, 0, 4, 4); glDrawArrays(GL_TRIANGLES, 0, 3);
         readpx(2, 2, px);
         printf("DIAG %s 4x4 MRT with a hole drew attachment 0 got %d %d %d %d want 255 0 0 255\n",
                px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255 ? "OK " : "BAD", px[0], px[1], px[2], px[3]);
      }
   }
   printf("DIAG-END\n");
   return 0;
}
