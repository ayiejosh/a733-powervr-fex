/* Border clamp sampling: 8x8 red texture with CLAMP_TO_BORDER + green border; sample centre (red)
 * and far outside (border colour). Compare native vs zink. */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_CLAMP_TO_BORDER_EXT
#define GL_CLAMP_TO_BORDER_EXT 0x812D
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint ok;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile %s\n",l);}return x;}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
   EGLint a=0,b=0; eglInitialize(d,&a,&b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0; EGLConfig cfg; EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   eglChooseConfig(d,ca,&cfg,1,&n); EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
   unsigned char px[8*8*4]; for(int i=0;i<8*8;i++){px[4*i]=255;px[4*i+1]=0;px[4*i+2]=0;px[4*i+3]=255;}
   GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_BORDER_EXT);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_BORDER_EXT);
   float border[4]={0,1,0,1};
   glTexParameterfv(GL_TEXTURE_2D,GL_TEXTURE_BORDER_COLOR, border);
   GLuint rt,fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64);
   const char*VS="attribute vec2 p; varying vec2 v; void main(){ v=p*0.5+0.5; gl_Position=vec4(p,0,1);}";
   const char*FS="precision mediump float; varying vec2 v; uniform sampler2D t; uniform vec2 off; void main(){ gl_FragColor=texture2D(t,v+off); }";
   GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,VS)); glAttachShader(p,sh(GL_FRAGMENT_SHADER,FS)); glLinkProgram(p); glUseProgram(p);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glUniform1i(glGetUniformLocation(p,"t"),0);
   static const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p"); glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts); glEnableVertexAttribArray(l);
   glUniform2f(glGetUniformLocation(p,"off"),0.0f,0.0f); glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char in[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,in);
   glUniform2f(glGetUniformLocation(p,"off"),1.5f,1.5f); glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char out[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);
   fprintf(stderr,"inside: %u %u %u %u | border: %u %u %u %u err 0x%x\n",in[0],in[1],in[2],in[3],out[0],out[1],out[2],out[3],glGetError());
   return 0;
}
