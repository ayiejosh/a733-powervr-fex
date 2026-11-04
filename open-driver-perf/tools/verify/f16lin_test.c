/* OES_texture_half_float_linear: a 4x4 half-float texture with columns 0,0,1,1 sampled with
 * GL_LINEAR at u=0.5 must average two texels (0.5 -> 128); GL_NEAREST shows one texel. */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);return x;}
static GLuint prog(const char*vs,const char*fs){GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char l[512];glGetProgramInfoLog(p,512,0,l);fprintf(stderr,"link %s\n",l);}return p;}
static void run(int linear, const char *what) {
   unsigned short data[4*4*4];
   /* half 0.0 = 0x0000, half 1.0 = 0x3c00; columns 0,1 -> 0.0, columns 2,3 -> 1.0 */
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
         unsigned short v = x < 2 ? 0x0000 : 0x3c00;
         for (int c = 0; c < 4; c++) data[(y*4+x)*4+c] = c == 3 ? 0x3c00 : v;
      }
   GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_HALF_FLOAT_OES,data);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
   GLuint rt; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   GLuint fbo; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   const char*vs="attribute vec2 p; varying vec2 v; void main(){ v=p*0.5+0.5; gl_Position=vec4(p,0,1); }";
   const char*fs="precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }";
   GLuint p=prog(vs,fs); glUseProgram(p);
   const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p"); glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts); glEnableVertexAttribArray(l);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glUniform1i(glGetUniformLocation(p,"t"),0);
   glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char px[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
   fprintf(stderr,"%s: %u %u %u %u err 0x%x\n",what,px[0],px[1],px[2],px[3],glGetError());
}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd?gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0):eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0,a[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   EGLConfig cfg;eglChooseConfig(d,a,&cfg,1,&n);EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa);eglMakeCurrent(d,0,0,c);
   const char*ext=(const char*)glGetString(GL_EXTENSIONS);
   fprintf(stderr,"half_float=%d half_float_linear=%d float_linear=%d\n",
      ext&&strstr(ext,"GL_OES_texture_half_float")!=NULL,
      ext&&strstr(ext,"GL_OES_texture_half_float_linear")!=NULL,
      ext&&strstr(ext,"GL_OES_texture_float_linear")!=NULL);
   run(0,"f16 nearest (expect 0 or 255)");
   run(1,"f16 linear  (expect 128)");
   return 0;}
