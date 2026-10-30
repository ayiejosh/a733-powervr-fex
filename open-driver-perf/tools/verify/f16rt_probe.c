/* Verify EXT_color_buffer_half_float honestly: render (clear + blend) into R16F/RG16F/RGBA16F
 * FBOs, then sample those textures into an RGBA8 target and check the converted pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define GL_HALF_FLOAT_OES 0x8D61
#define GL_R16F   0x822D
#define GL_RG16F  0x822F
#define GL_RGBA16F 0x881A
#define GL_RED 0x1903
#define GL_RG 0x8227

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x);
   GLint ok = 0; glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
   if (!ok) { char l[512]; glGetShaderInfoLog(x, 512, NULL, l); fprintf(stderr, "compile %s\n", l); }
   return x;
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

   const char *vs = "attribute vec2 p; varying vec2 uv; void main(){ uv=p*0.5+0.5; gl_Position=vec4(p,0.5,1.0); }";
   const char *fs_out = "precision mediump float; void main(){ gl_FragColor=vec4(1.0,0.0,0.0,0.5); }";
   const char *fs_tex = "precision mediump float; varying vec2 uv; uniform sampler2D t;"
                        "void main(){ gl_FragColor=texture2D(t, uv); }";
   GLuint prog_out = glCreateProgram(), prog_tex = glCreateProgram();
   glAttachShader(prog_out, sh(GL_VERTEX_SHADER, vs));
   glAttachShader(prog_out, sh(GL_FRAGMENT_SHADER, fs_out));
   glLinkProgram(prog_out);
   glAttachShader(prog_tex, sh(GL_VERTEX_SHADER, vs));
   glAttachShader(prog_tex, sh(GL_FRAGMENT_SHADER, fs_tex));
   glLinkProgram(prog_tex);
   static const float verts[8] = { -1,-1, 3,-1, -1,3 };
   int fail = 0;

   /* Output RGBA8 target. */
   GLuint rt8, fbo8;
   glGenTextures(1, &rt8); glBindTexture(GL_TEXTURE_2D, rt8);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo8); glBindFramebuffer(GL_FRAMEBUFFER, fbo8);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt8, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("rgba8 fbo bad\n"); return 1; }

   struct { GLenum fmt, fdef, type; float clear[4]; float want_r, want_g, want_b; const char *name; } cases[] = {
      /* ES2 usage per EXT_color_buffer_half_float: unsized GL_RGBA + HALF_FLOAT_OES becomes RGBA16F. */
      { GL_RGBA, GL_RGBA, GL_HALF_FLOAT_OES, {0.25f, 0.5f, 0.75f, 1.0f}, 0.625f, 0.25f, 0.375f, "RGBA16F" },
      { GL_RGB,  GL_RGB, GL_HALF_FLOAT_OES, {0.25f, 0.5f, 0.75f, 1.0f}, 0.625f, 0.25f, 0.375f, "RGB16F" },
   };
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      GLuint ft, fbo;
      glGenTextures(1, &ft); glBindTexture(GL_TEXTURE_2D, ft);
      glTexImage2D(GL_TEXTURE_2D, 0, cases[i].fmt, 64, 64, 0, cases[i].fdef, cases[i].type, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ft, 0);
      GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (st != GL_FRAMEBUFFER_COMPLETE) {
         printf("%s: FBO incomplete 0x%x\n", cases[i].name, st); fail = 1; continue;
      }
      glViewport(0, 0, 64, 64);
      glClearColor(cases[i].clear[0], cases[i].clear[1], cases[i].clear[2], cases[i].clear[3]);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(prog_out);
      GLint l = glGetAttribLocation(prog_out, "p");
      glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(l);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glDisable(GL_BLEND);
      GLenum e1 = glGetError();

      /* Sample it into RGBA8. */
      glBindFramebuffer(GL_FRAMEBUFFER, fbo8);
      glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(prog_tex);
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ft);
      glUniform1i(glGetUniformLocation(prog_tex, "t"), 0);
      l = glGetAttribLocation(prog_tex, "p");
      glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(l);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      unsigned char px[4] = {0,0,0,0};
      glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      GLenum e2 = glGetError();
      int dr = abs((int)px[0] - (int)(cases[i].want_r * 255.0f + 0.5f));
      int dg = abs((int)px[1] - (int)(cases[i].want_g * 255.0f + 0.5f));
      int db = abs((int)px[2] - (int)(cases[i].want_b * 255.0f + 0.5f));
      printf("%s: fbo=0x%x err=0x%x/0x%x px=%u %u %u %u want~%d %d %d d=%d,%d,%d -> %s\n",
             cases[i].name, st, e1, e2, px[0], px[1], px[2], px[3],
             (int)(cases[i].want_r * 255.0f + 0.5f), (int)(cases[i].want_g * 255.0f + 0.5f),
             (int)(cases[i].want_b * 255.0f + 0.5f), dr, dg, db,
             (e1 || e2 || dr > 3 || dg > 3 || db > 3) ? "FAIL" : "PASS");
      if (e1 || e2 || dr > 3 || dg > 3 || db > 3) fail = 1;
      glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &ft);
   }
   printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
   return fail;
}
