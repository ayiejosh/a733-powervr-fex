/* glPolygonOffset probe v3. FBO colour+depth(D16), clear depth 0.5, draw quad at z=0.5:
 *  A ALWAYS            -> green (setup sanity)
 *  B LEQUAL no offset  -> green (depth buffer works)
 *  C LESS no offset    -> black (equal depth rejected)
 *  D LESS offset big   -> green iff polygon offset works
 *  E LESS offset small -> green iff units scaling works */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#endif
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
   EGLint a = 0, b = 0; eglInitialize(d, &a, &b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n = 0; EGLConfig cfg;
   EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
   eglChooseConfig(d, ca, &cfg, 1, &n);
   EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
   GLuint rt, depth, fbo;
   glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenTextures(1, &depth); glBindTexture(GL_TEXTURE_2D, depth);
   if (getenv("DEPTH32")) {
      void (*ts)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) =
         (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei))eglGetProcAddress("glTexStorage2DEXT");
      if (ts) ts(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 64, 64);
      else fprintf(stderr, "no glTexStorage2DEXT\n");
   } else {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 64, 64, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL);
   }
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
   fprintf(stderr, "fbo=0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
   const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p, -0.5, 1.0); }";
   const char *fs = "precision mediump float; void main(){ gl_FragColor = vec4(0.0,1.0,0.0,1.0); }";
   GLuint prog = glCreateProgram();
   glAttachShader(prog, sh(GL_VERTEX_SHADER, vs));
   glAttachShader(prog, sh(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(prog);
   glUseProgram(prog);
   static const float verts[8] = { -1,-1, 3,-1, -1,3 };
   GLint l = glGetAttribLocation(prog, "p");
   glVertexAttribPointer(l, 2, GL_FLOAT, GL_FALSE, 0, verts);
   glEnableVertexAttribArray(l);
   glViewport(0, 0, 64, 64);
   glEnable(GL_DEPTH_TEST);
   unsigned char px[4];
   const char *names[] = { "A always            ", "B less no offset    ", "C less off +40000   ",
                           "D less off -40000   ", "E less off +2       " };
   GLenum funcs[] = { GL_ALWAYS, GL_LESS, GL_LESS, GL_LESS, GL_LESS };
   int offsets[] = { 0, 0, 1, 1, 1 };
   float u = getenv("UNITS") ? (float)atof(getenv("UNITS")) : (getenv("BIGOFF") ? 16000000.0f : 40000.0f);
   float vals[] = { 0, 0, u, -u, 2.0f };
   for (int i = 0; i < 5; i++) {
      glDepthFunc(funcs[i]);
      if (offsets[i]) { glEnable(GL_POLYGON_OFFSET_FILL);
         float fac = getenv("FACTOR") ? (float)atof(getenv("FACTOR")) : 0.0f;
         if (getenv("CLAMP")) {
            void (*pgoc)(float, float, float) =
               (void (*)(float, float, float))eglGetProcAddress("glPolygonOffsetClampEXT");
            if (pgoc) pgoc(fac, vals[i], (float)atof(getenv("CLAMP")));
            else fprintf(stderr, "no glPolygonOffsetClampEXT\n");
         } else glPolygonOffset(fac, vals[i]);
      }
      else glDisable(GL_POLYGON_OFFSET_FILL);
      glClearColor(0, 0, 0, 1); glClearDepthf(0.5f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      px[0] = px[1] = px[2] = px[3] = 0;
      glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "%s: %u %u %u %u\n", names[i], px[0], px[1], px[2], px[3]);
   }
   fprintf(stderr, "err 0x%x\n", glGetError());
   return 0;
}
