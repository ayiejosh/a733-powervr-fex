/* GL_EXT_texture_mirror_clamp_to_edge: sample a 4x1 texture with MIRROR_CLAMP_TO_EDGE at u=-0.25 and
 * u=0.5; expected (0.5 samples texel1). Compare native vs zink. */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#ifndef GL_MIRROR_CLAMP_TO_EDGE_EXT
#define GL_MIRROR_CLAMP_TO_EDGE_EXT 0x8743
#endif
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint ok;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile %s\n",l);}return x;}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
   EGLint a=0,b=0; eglInitialize(d,&a,&b); eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0; EGLConfig cfg; EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   eglChooseConfig(d,ca,&cfg,1,&n); EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c);
   /* 4x1 texture: texel0 = (64,0,0), texel1 = (200,0,0), texel2 = (64,0,0), texel3 = (200,0,0) */
   unsigned char px[4*4]={64,0,0,255, 200,0,0,255, 64,0,0,255, 200,0,0,255};
   GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,1,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_MIRROR_CLAMP_TO_EDGE_EXT);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_MIRROR_CLAMP_TO_EDGE_EXT);
   GLuint rt,fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64);
   const char*VS="attribute vec2 p; varying vec2 v; void main(){ v=p*0.5+0.5; gl_Position=vec4(p,0,1);}";
   const char*FS="precision mediump float; varying vec2 v; uniform sampler2D t; uniform vec2 off; void main(){ gl_FragColor=texture2D(t,v*vec2(0.25,0.0)+off); }";
   GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,VS)); glAttachShader(p,sh(GL_FRAGMENT_SHADER,FS)); glLinkProgram(p); glUseProgram(p);
   glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex); glUniform1i(glGetUniformLocation(p,"t"),0);
   static const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p"); glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts); glEnableVertexAttribArray(l);
   glUniform2f(glGetUniformLocation(p,"off"),0.5f,0.5f); glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char mid[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,mid);
   glUniform2f(glGetUniformLocation(p,"off"),-0.525f,0.5f); glDrawArrays(GL_TRIANGLES,0,3);
   unsigned char neg[4]={0}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,neg);
   fprintf(stderr,"mid: %u %u %u %u | mirrored(-0.25): %u %u %u %u err 0x%x\n",mid[0],mid[1],mid[2],mid[3],neg[0],neg[1],neg[2],neg[3],glGetError());
   return 0;
}
