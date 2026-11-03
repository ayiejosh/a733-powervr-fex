/* Gate probe (fragment coordinates): gl_FragCoord.xy must be the pixel centre (x + 0.5, y + 0.5) in GL
 * window coordinates, so a shader that derives a texture coordinate from it samples the right texel.
 * 64x64 RGBA target:
 *  F1 fract(gl_FragCoord.xy) must be 0.5 (reads 128 128)
 *  F2 floor(gl_FragCoord.xy) at pixel (x,y) must be (x,y) (written as x,y bytes)
 *  F3 upload a pattern, attach the texture to an FBO, glReadPixels: the upload + readback round trip
 *     through the staging copies, no normalized sampling
 *  F4 sample the uploaded pattern at texel centres (x+0.5)/64 computed from floor(gl_FragCoord)+0.5,
 *     i.e. independent of the fragment coordinate's fraction
 *  F5 F2 on a 64x64 pbuffer (window-system framebuffer, drawn y-flipped) instead of an FBO
 * RGBX_ANY_RENDERER=1 allows a reference driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static unsigned char pr(int x, int y) { return (unsigned char)(x * 7 + y * 3 + 1); }
static unsigned char pg(int x, int y) { return (unsigned char)((x * 13) ^ (y * 5)); }
static unsigned char pb(int x, int y) { return (unsigned char)(x + y * 11); }
static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x); return x; }
static GLuint prog(const char *fs)
{
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 q; void main(){ gl_Position=vec4(q,0.0,1.0); }"));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "q"); glLinkProgram(p); return p;
}
static void px(int x, int y, unsigned char o[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o); }

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
   if (!getenv("RGBX_ANY_RENDERER") && (rn == NULL || vn == NULL || strncmp(rn, "PowerVR ", 8) != 0 ||
                                        strstr(rn, "zink") || strstr(vn, "Mesa") == NULL))
      { printf("DIAG wrong renderer\n"); return 1; }
   static const float verts[6] = { -1, -1, 3, -1, -1, 3 };
   GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   GLuint dst, fbo;
   glGenTextures(1, &dst); glBindTexture(GL_TEXTURE_2D, dst);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
   glViewport(0, 0, 64, 64);
   unsigned char o[4];
   /* F1 */
   GLuint p1 = prog("precision highp float; void main(){ gl_FragColor=vec4(fract(gl_FragCoord.xy), 0.0, 1.0); }");
   glUseProgram(p1); glDrawArrays(GL_TRIANGLES, 0, 3);
   px(5, 9, o); printf("DIAG %s F1 fract(gl_FragCoord) at 5,9 = %d %d (want 128 128)\n", (o[0] == 128 || o[0] == 127) && (o[1] == 128 || o[1] == 127) ? "OK " : "BAD", o[0], o[1]);
   /* F2 */
   GLuint p2 = prog("precision highp float; void main(){ gl_FragColor=vec4(floor(gl_FragCoord.xy) / 255.0, 0.0, 1.0); }");
   glUseProgram(p2); glDrawArrays(GL_TRIANGLES, 0, 3);
   px(5, 9, o); printf("DIAG %s F2 floor(gl_FragCoord) at 5,9 = %d %d (want 5 9)\n", o[0] == 5 && o[1] == 9 ? "OK " : "BAD", o[0], o[1]);
   px(0, 0, o); printf("DIAG %s F2 floor(gl_FragCoord) at 0,0 = %d %d (want 0 0)\n", o[0] == 0 && o[1] == 0 ? "OK " : "BAD", o[0], o[1]);
   /* F3: upload + readback round trip */
   unsigned char *data = malloc(64 * 64 * 4);
   for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
      unsigned char *t = data + (y * 64 + x) * 4; t[0] = pr(x, y); t[1] = pg(x, y); t[2] = pb(x, y); t[3] = 255; }
   GLuint src, f2;
   glGenTextures(1, &src); glBindTexture(GL_TEXTURE_2D, src);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &f2); glBindFramebuffer(GL_FRAMEBUFFER, f2);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
   const int pts[][2] = { {0, 0}, {5, 9}, {40, 33}, {63, 63} };
   for (int i = 0; i < 4; i++) {
      px(pts[i][0], pts[i][1], o);
      printf("DIAG %s F3 upload+readback at %d,%d = %d %d %d (want %d %d %d)\n",
             o[0] == pr(pts[i][0], pts[i][1]) && o[1] == pg(pts[i][0], pts[i][1]) && o[2] == pb(pts[i][0], pts[i][1]) ? "OK " : "BAD",
             pts[i][0], pts[i][1], o[0], o[1], o[2], pr(pts[i][0], pts[i][1]), pg(pts[i][0], pts[i][1]), pb(pts[i][0], pts[i][1]));
   }
   /* F4: sample at texel centres computed from floor(gl_FragCoord) */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   GLuint p4 = prog("precision highp float; uniform sampler2D t;"
                    " void main(){ gl_FragColor=texture2D(t, (floor(gl_FragCoord.xy) + 0.5) / 64.0); }");
   glUseProgram(p4); glUniform1i(glGetUniformLocation(p4, "t"), 0);
   glBindTexture(GL_TEXTURE_2D, src); glDrawArrays(GL_TRIANGLES, 0, 3);
   for (int i = 0; i < 4; i++) {
      px(pts[i][0], pts[i][1], o);
      printf("DIAG %s F4 sample at texel centre %d,%d = %d %d %d (want %d %d %d)\n",
             o[0] == pr(pts[i][0], pts[i][1]) && o[1] == pg(pts[i][0], pts[i][1]) && o[2] == pb(pts[i][0], pts[i][1]) ? "OK " : "BAD",
             pts[i][0], pts[i][1], o[0], o[1], o[2], pr(pts[i][0], pts[i][1]), pg(pts[i][0], pts[i][1]), pb(pts[i][0], pts[i][1]));
   }
   /* F5: F2 on a pbuffer, the window-system framebuffer the state tracker draws y-flipped */
   EGLint pa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface ps = eglCreatePbufferSurface(d, cfg, pa);
   if (ps == EGL_NO_SURFACE || !eglMakeCurrent(d, ps, ps, c)) {
      printf("DIAG BAD F5 no pbuffer surface (EGL error 0x%x)\n", eglGetError());
   } else {
      glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, 64, 64);
      glUseProgram(p2); glDrawArrays(GL_TRIANGLES, 0, 3);
      px(5, 9, o); printf("DIAG %s F5 pbuffer floor(gl_FragCoord) at 5,9 = %d %d (want 5 9)\n", o[0] == 5 && o[1] == 9 ? "OK " : "BAD", o[0], o[1]);
      px(0, 63, o); printf("DIAG %s F5 pbuffer floor(gl_FragCoord) at 0,63 = %d %d (want 0 63)\n", o[0] == 0 && o[1] == 63 ? "OK " : "BAD", o[0], o[1]);
   }
   printf("DIAG-END\n");
   return 0;
}
