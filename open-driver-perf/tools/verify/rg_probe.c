/* GL_EXT_texture_rg: sample R8 and RG8 textures, compare native vs zink. Expect (r,0,0,1)/(r,g,0,1). */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_R8_EXT
#define GL_R8_EXT 0x8229
#define GL_RG8_EXT 0x822B
#define GL_RED_EXT 0x1903
#define GL_RG_EXT 0x8227
#endif
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint ok;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile %s\n",l);}return x;}
static GLuint prog(const char*vs,const char*fs){GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char l[512];glGetProgramInfoLog(p,512,0,l);fprintf(stderr,"link %s\n",l);}return p;}
static const char*VS="attribute vec2 p; varying vec2 v; void main(){ v=p*0.5+0.5; gl_Position=vec4(p,0,1);}";
static const char*FS="precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor=texture2D(t,v); }";
static void draw(GLuint tex){
   GLuint rt,fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   GLuint p=prog(VS,FS); glUseProgram(p); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
   glUniform1i(glGetUniformLocation(p,"t"),0);
   static const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p"); glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts); glEnableVertexAttribArray(l);
   glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char px[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
   fprintf(stderr,"px: %u %u %u %u err 0x%x\n",px[0],px[1],px[2],px[3],glGetError());
}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
   EGLint a=0,b=0; eglInitialize(d,&a,&b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0; EGLConfig cfg; EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   eglChooseConfig(d,ca,&cfg,1,&n); EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
   fprintf(stderr,"renderer: %s\n", glGetString(GL_RENDERER));
   unsigned char r8[64*64]; for(int i=0;i<64*64;i++) r8[i]=128;
   GLuint t8; glGenTextures(1,&t8); glBindTexture(GL_TEXTURE_2D,t8);
   glTexImage2D(GL_TEXTURE_2D,0,GL_R8_EXT,64,64,0,GL_RED_EXT,GL_UNSIGNED_BYTE,r8);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   fprintf(stderr,"R8  "); draw(t8);
   unsigned char rg[64*64*2]; for(int i=0;i<64*64;i++){rg[2*i]=128;rg[2*i+1]=64;}
   GLuint t16; glGenTextures(1,&t16); glBindTexture(GL_TEXTURE_2D,t16);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RG8_EXT,64,64,0,GL_RG_EXT,GL_UNSIGNED_BYTE,rg);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   fprintf(stderr,"RG8 "); draw(t16);
   return 0;
}
