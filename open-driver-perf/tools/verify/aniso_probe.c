/* EXT_texture_filter_anisotropic: 256x256 checker with mips, linear mip filtering, anisotropic
 * footprint (x compressed 8x, y 0.5x); sample centre with max anisotropy 1 vs 16 and report. */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint ok;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile %s\n",l);}return x;}
static unsigned char px[256*256*4];
static void draw(GLuint p,GLuint tex,float aniso,unsigned char *out){
   glBindTexture(GL_TEXTURE_2D,tex);
   glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,aniso);
   GLuint rt,fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64); glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(p); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
   glUniform1i(glGetUniformLocation(p,"t"),0);
   static const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p"); glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts); glEnableVertexAttribArray(l);
   glDrawArrays(GL_TRIANGLES,0,3);
   glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);
}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
   EGLint a=0,b=0; eglInitialize(d,&a,&b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0; EGLConfig cfg; EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   eglChooseConfig(d,ca,&cfg,1,&n); EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
   for(int y=0;y<256;y++) for(int x=0;x<256;x++){ int v=((x/8)+(y/8))&1; unsigned char*p=&px[(y*256+x)*4];
      p[0]=v?255:0; p[1]=v?255:0; p[2]=v?255:0; p[3]=255; }
   GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
   glGenerateMipmap(GL_TEXTURE_2D);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
   GLfloat maxaniso=0; glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&maxaniso);
   const char*VS="attribute vec2 p; varying vec2 v; void main(){ v=p*0.5+0.5; gl_Position=vec4(p,0,1);}";
   const char*FS="precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor=texture2D(t, vec2(v.x*8.0, v.y*0.5)); }";
   GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,VS)); glAttachShader(p,sh(GL_FRAGMENT_SHADER,FS)); glLinkProgram(p);
   unsigned char a1[4]={0}, a16[4]={0};
   draw(p,tex,1.0f,a1); draw(p,tex,16.0f,a16);
   fprintf(stderr,"maxaniso=%.1f aniso1=%u,%u,%u aniso16=%u,%u,%u err 0x%x\n",maxaniso,a1[0],a1[1],a1[2],a16[0],a16[1],a16[2],glGetError());
   return 0;
}
