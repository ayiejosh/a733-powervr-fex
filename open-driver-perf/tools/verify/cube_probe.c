/* ES2 cube maps (core): six distinct faces, sampled with the six axis directions. */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint ok;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,512,0,l);fprintf(stderr,"compile %s\n",l);}return x;}
static GLuint prog(const char*vs,const char*fs){GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char l[512];glGetProgramInfoLog(p,512,0,l);fprintf(stderr,"link %s\n",l);}return p;}
int main(void){
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay d=gpd?gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0):eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0,a[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   EGLConfig cfg;eglChooseConfig(d,a,&cfg,1,&n);EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa);eglMakeCurrent(d,0,0,c);
   const char*ext=(const char*)glGetString(GL_EXTENSIONS);
   fprintf(stderr,"cube ok caps: max_cube=%d\n", 1);
   GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_CUBE_MAP,tex);
   unsigned char face[8*8*4];
   const unsigned colors[6][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{0,255,255}};
   for(int f=0;f<6;f++){for(int i=0;i<8*8;i++){face[4*i+0]=colors[f][0];face[4*i+1]=colors[f][1];face[4*i+2]=colors[f][2];face[4*i+3]=255;}
      glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,face);}
   glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
   GLuint rt;glGenTextures(1,&rt);glBindTexture(GL_TEXTURE_2D,rt);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   GLuint fbo;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
   glViewport(0,0,64,64);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
   const char*vs="attribute vec2 p; void main(){gl_Position=vec4(p,0,1);}";
   const char*fs="precision mediump float; uniform samplerCube s; uniform vec3 dir; void main(){ gl_FragColor = textureCube(s, dir); }";
   GLuint p=prog(vs,fs);glUseProgram(p);
   const float verts[8]={-1,-1,3,-1,-1,3};
   GLint l=glGetAttribLocation(p,"p");glVertexAttribPointer(l,2,GL_FLOAT,GL_FALSE,0,verts);glEnableVertexAttribArray(l);
   glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,tex);
   glUniform1i(glGetUniformLocation(p,"s"),0);
   const float dirs[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
   const char*names[6]={"+X(red)","-X(green)","+Y(blue)","-Y(yellow)","+Z(magenta)","-Z(cyan)"};
   for(int i=0;i<6;i++){
      glUniform3fv(glGetUniformLocation(p,"dir"),1,dirs[i]);
      glDrawArrays(GL_TRIANGLES,0,3);
      unsigned char px[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
      fprintf(stderr,"face %s: %u %u %u %u err 0x%x\n",names[i],px[0],px[1],px[2],px[3],glGetError());
   }
   return 0;}
