// Comprehensive GLES 3.1/3.2 benchmark — emits real measured numbers.
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);GLint k;glGetShaderiv(x,GL_COMPILE_STATUS,&k);if(!k){char l[800];glGetShaderInfoLog(x,800,0,l);fprintf(stderr,"shc: %s\n",l);}return x;}
static GLuint prog(const char*vs,const char*fs){GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));if(fs)glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));glLinkProgram(p);GLint k;glGetProgramiv(p,GL_LINK_STATUS,&k);if(!k){char l[800];glGetProgramInfoLog(p,800,0,l);fprintf(stderr,"link: %s\n",l);}return p;}
#define W 1024
#define H 1024
int main(){
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); if(!eglInitialize(d,0,0)){printf("init fail\n");return 1;}
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint a[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};EGLConfig c;EGLint n;eglChooseConfig(d,a,&c,1,&n);
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,ca);eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx);
  printf("# renderer: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  GLuint fbo,tex;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
  glViewport(0,0,W,H);
  GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
  float quad[]={-1,-1, 1,-1, -1,1,  -1,1, 1,-1, 1,1};
  float tri[]={-0.01f,-0.01f, 0.01f,-0.01f, 0,0.01f};
  GLuint vb;glGenBuffers(1,&vb);glBindBuffer(GL_ARRAY_BUFFER,vb);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,0,0,0);
  const char*VS="#version 310 es\nlayout(location=0)in vec2 P;void main(){gl_Position=vec4(P,0,1);}";
  GLuint pSimple=prog(VS,"#version 310 es\nprecision mediump float;out vec4 C;void main(){C=vec4(1,.5,0,1);}");
  GLuint pAlu=prog(VS,"#version 310 es\nprecision highp float;out vec4 C;void main(){vec2 u=gl_FragCoord.xy*0.01;float s=0.0;for(int i=0;i<100;i++){s+=sin(u.x*float(i))*cos(u.y*float(i));u=u*1.01+0.13;}C=vec4(s,s*0.5,s*0.25,1);}");
  // texture for sampling
  unsigned char* td=malloc(256*256*4); for(int i=0;i<256*256*4;i++) td[i]=i&0xff;
  GLuint stex;glGenTextures(1,&stex);glBindTexture(GL_TEXTURE_2D,stex);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,td);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  GLuint pTex=prog("#version 310 es\nlayout(location=0)in vec2 P;out vec2 uv;void main(){uv=P*4.0;gl_Position=vec4(P,0,1);}",
                   "#version 310 es\nprecision mediump float;in vec2 uv;uniform sampler2D s;out vec4 C;void main(){C=texture(s,uv);}");
  double t; int i;
  // warmup
  glUseProgram(pSimple);for(i=0;i<10;i++)glDrawArrays(GL_TRIANGLES,0,6);glFinish();

  // 1) glClear call rate
  {int N=30000;glFinish();t=now();for(i=0;i<N;i++)glClear(GL_COLOR_BUFFER_BIT);glFinish();double dt=now()-t;printf("call.glClear.kps          %.0f\n",N/dt/1e3);}
  // 2) draw-call rate (tiny tri)
  {glBindBuffer(GL_ARRAY_BUFFER,vb);glBufferData(GL_ARRAY_BUFFER,sizeof(tri),tri,GL_STATIC_DRAW);glVertexAttribPointer(0,2,GL_FLOAT,0,0,0);
   int N=30000;glFinish();t=now();for(i=0;i<N;i++)glDrawArrays(GL_TRIANGLES,0,3);glFinish();double dt=now()-t;printf("call.draw.kps             %.0f\n",N/dt/1e3);}
  // 3) fill rate (simple frag, fullscreen)
  {glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);glVertexAttribPointer(0,2,GL_FLOAT,0,0,0);glUseProgram(pSimple);
   int M=3000;glFinish();t=now();for(i=0;i<M;i++)glDrawArrays(GL_TRIANGLES,0,6);glFinish();double dt=now()-t;printf("fill.simple.Mpix_s        %.0f\n",(double)M*W*H/dt/1e6);}
  // 4) shader-ALU throughput (heavy frag, 100-iter loop)
  {glUseProgram(pAlu);int M=200;glFinish();t=now();for(i=0;i<M;i++)glDrawArrays(GL_TRIANGLES,0,6);glFinish();double dt=now()-t;
   printf("shader.alu.Mpix_s         %.0f\n",(double)M*W*H/dt/1e6);
   printf("shader.alu.GFLOP_s        %.1f\n",(double)M*W*H*100*4/dt/1e9);} // ~4 flop/iter *100
  // 5) texture sampling rate
  {glUseProgram(pTex);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,stex);
   int M=1500;glFinish();t=now();for(i=0;i<M;i++)glDrawArrays(GL_TRIANGLES,0,6);glFinish();double dt=now()-t;printf("texture.Mtexel_s          %.0f\n",(double)M*W*H/dt/1e6);}
  // 6) triangle rate (many tiny tris, geometry-bound)
  {int NT=20000;float* tb=malloc(NT*6*sizeof(float));for(i=0;i<NT;i++){float x=((i%200)/200.0f)*2-1;tb[i*6]=x;tb[i*6+1]=-1;tb[i*6+2]=x+0.005f;tb[i*6+3]=-1;tb[i*6+4]=x;tb[i*6+5]=-0.99f;}
   glBufferData(GL_ARRAY_BUFFER,NT*6*sizeof(float),tb,GL_STATIC_DRAW);glVertexAttribPointer(0,2,GL_FLOAT,0,0,0);glUseProgram(pSimple);
   int M=200;glFinish();t=now();for(i=0;i<M;i++)glDrawArrays(GL_TRIANGLES,0,NT*3);glFinish();double dt=now()-t;printf("triangle.Mtri_s           %.1f\n",(double)M*NT/dt/1e6);free(tb);}
  // 7) compute throughput (GLES 3.1 SSBO write)
  {GLuint cp=glCreateProgram();glAttachShader(cp,sh(GL_COMPUTE_SHADER,"#version 310 es\nlayout(local_size_x=256)in;layout(std430,binding=0)buffer B{uint o[];};void main(){uint i=gl_GlobalInvocationID.x;uint v=i;for(int k=0;k<64;k++)v=v*1664525u+1013904223u;o[i]=v;}"));glLinkProgram(cp);glUseProgram(cp);
   int NE=1<<20;GLuint sb;glGenBuffers(1,&sb);glBindBuffer(GL_SHADER_STORAGE_BUFFER,sb);glBufferData(GL_SHADER_STORAGE_BUFFER,NE*4,0,GL_DYNAMIC_COPY);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,sb);
   int M=300;glFinish();t=now();for(i=0;i<M;i++){glDispatchCompute(NE/256,1,1);glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);}glFinish();double dt=now()-t;
   printf("compute.Melem_s           %.0f\n",(double)M*NE/dt/1e6);
   printf("compute.GIntOp_s          %.1f\n",(double)M*NE*64*2/dt/1e9);}
  // 8) buffer upload bandwidth
  {int SZ=4<<20;void*buf=malloc(SZ);memset(buf,1,SZ);GLuint ub;glGenBuffers(1,&ub);glBindBuffer(GL_ARRAY_BUFFER,ub);
   int M=200;glFinish();t=now();for(i=0;i<M;i++)glBufferData(GL_ARRAY_BUFFER,SZ,buf,GL_STREAM_DRAW);glFinish();double dt=now()-t;printf("upload.buffer.GB_s        %.2f\n",(double)M*SZ/dt/1e9);free(buf);}
  // 9) texture upload bandwidth
  {int TW=512;void*buf=malloc(TW*TW*4);memset(buf,1,TW*TW*4);GLuint ut;glGenTextures(1,&ut);glBindTexture(GL_TEXTURE_2D,ut);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,TW,TW,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
   int M=300;glFinish();t=now();for(i=0;i<M;i++)glTexSubImage2D(GL_TEXTURE_2D,0,0,0,TW,TW,GL_RGBA,GL_UNSIGNED_BYTE,buf);glFinish();double dt=now()-t;printf("upload.texture.GB_s       %.2f\n",(double)M*TW*TW*4/dt/1e9);free(buf);}
  printf("glerr 0x%x\n",glGetError());
  return 0;
}
