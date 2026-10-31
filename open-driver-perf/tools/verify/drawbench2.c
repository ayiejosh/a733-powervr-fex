/* drawbench2: fixed-work per-feature GPU microbenchmark on surfaceless EGL, so native (our Mesa +
 * pvr_native) and zink (same Vulkan open driver) run the same code without X/Wayland. Modes measure
 * fill rate, texture fetch (nearest/linear), mipmapped fetch, cube fetch and vertex throughput. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);
   GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[2048];glGetShaderInfoLog(x,sizeof l,0,l);fprintf(stderr,"compile: %s\n",l);exit(2);}return x;}
static GLuint prog(const char*vs,const char*fs){GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));glLinkProgram(p);
   GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char l[2048];glGetProgramInfoLog(p,sizeof l,0,l);fprintf(stderr,"link: %s\n",l);exit(2);}return p;}
#define W 800
#define H 600
static const char *VS_FULL = "attribute vec2 p; varying vec2 v; void main(){ v = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }";
static const char *VS_GRID = "attribute vec2 p; uniform float s; void main(){ gl_Position = vec4(p*s, 0.0, 1.0); }";
int main(void){
   const char *mode = getenv("MODE") ? getenv("MODE") : "fill";
   const int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 200;
   const int nverts = getenv("VERTS") ? atoi(getenv("VERTS")) : 60000;
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj,min; if(!eglInitialize(dpy,&maj,&min)){fprintf(stderr,"egl init\n");return 2;}
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint n=0; EGLConfig cfg=EGL_NO_CONFIG_KHR;
   EGLint a[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
   eglChooseConfig(dpy,a,&cfg,1,&n);
   EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
   EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,xa);
   if(ctx==EGL_NO_CONTEXT || !eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)){fprintf(stderr,"ctx\n");return 2;}
   GLuint fbo,ctex,rdtex;
   glGenTextures(1,&ctex); glBindTexture(GL_TEXTURE_2D,ctex);
   unsigned char px[512*512*4]; for(int i=0;i<512*512;i++){px[4*i]= (i*7)&255; px[4*i+1]=(i*13)&255; px[4*i+2]=(i*29)&255; px[4*i+3]=255;}
   int mips = !strncmp(mode,"mip",3);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
   if(mips) glGenerateMipmap(GL_TEXTURE_2D);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER, mips ? (strstr(mode,"linear")?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST_MIPMAP_NEAREST) : (strstr(mode,"linear")?GL_LINEAR:GL_NEAREST));
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER, strstr(mode,"linear")?GL_LINEAR:GL_NEAREST);
   glGenTextures(1,&rdtex); glBindTexture(GL_TEXTURE_2D,rdtex);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
   glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rdtex,0);
   glViewport(0,0,W,H);
   const int npasses = getenv("NPASSES") ? atoi(getenv("NPASSES")) : 27;
   const int psize = getenv("PSIZE") ? atoi(getenv("PSIZE")) : 64;
   GLuint small_tex = 0, small_fbo = 0, alt_tex = 0;
   if (!strcmp(mode,"passes") || !strcmp(mode,"manydraws") || !strcmp(mode,"clearonly") || !strcmp(mode,"alt") || !strcmp(mode,"blend") || !strcmp(mode,"blenddepth")) {
      glGenTextures(1,&small_tex); glBindTexture(GL_TEXTURE_2D,small_tex);
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,psize,psize,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
      if (!strcmp(mode,"alt")) {
         glGenTextures(1,&alt_tex); glBindTexture(GL_TEXTURE_2D,alt_tex);
         glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,psize,psize,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
      }
      glGenFramebuffers(1,&small_fbo);
   }
   GLuint p; int verts = 0;
   const char *fs_fill = "precision mediump float; void main(){ gl_FragColor = vec4(0.5,0.25,0.75,1.0); }";
   const char *fs_tex  = "precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }";
   const char *fs_cube = "precision mediump float; varying vec2 v; uniform samplerCube t; void main(){ gl_FragColor = textureCube(t, vec3(v*2.0-1.0, 1.0)); }";
   GLuint cube=0;
   if (!strcmp(mode,"fill")||!strcmp(mode,"overdraw")) p = prog(VS_FULL, fs_fill);
   else if (!strcmp(mode,"cube")) {
      glGenTextures(1,&cube); glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
      for(int f=0;f<6;f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
      glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      p = prog(VS_FULL, fs_cube);
   } else if (!strcmp(mode,"vert")) { p = prog(VS_GRID, fs_fill); verts = nverts; }
   else if (!strcmp(mode,"passes") || !strcmp(mode,"manydraws") || !strcmp(mode,"clearonly") || !strcmp(mode,"alt") || !strcmp(mode,"blend") || !strcmp(mode,"blenddepth")) p = prog(VS_FULL, fs_fill);
   else p = prog(VS_FULL, fs_tex);
   glUseProgram(p);
   GLuint bo=0;
   const float quad[8]={-1,-1, 3,-1, -1,3};
   float *grid = NULL;
   if (verts) {
      grid = malloc(sizeof(float)*2*verts);
      for(int i=0;i<verts/3;i++){ float x=-1.0f+2.0f*(i%200)/200.0f, y=-1.0f+2.0f*(i/200)/(float)(verts/600+1);
         for(int k=0;k<3;k++){ grid[2*(3*i+k)]=x+0.004f*(k==1); grid[2*(3*i+k)+1]=y+0.004f*(k==2); } }
      glGenBuffers(1,&bo); glBindBuffer(GL_ARRAY_BUFFER,bo);
      glBufferData(GL_ARRAY_BUFFER,sizeof(float)*2*verts,grid,GL_STATIC_DRAW);
      glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0); glEnableVertexAttribArray(0);
      glBindAttribLocation(p,0,"p");
   } else {
      glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad); glEnableVertexAttribArray(0);
      glBindAttribLocation(p,0,"p");
   }
   glUseProgram(p);
   if (cube) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_CUBE_MAP,cube); glUniform1i(glGetUniformLocation(p,"t"),0); }
   else if (!verts && strcmp(mode,"fill") && strcmp(mode,"overdraw")) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,ctex); glUniform1i(glGetUniformLocation(p,"t"),0); }
   glClearColor(0,0,0,1);
   double t0=now();
   for(int f=0; f<frames; f++){
      if (!strcmp(mode,"passes") || !strcmp(mode,"clearonly") || !strcmp(mode,"alt")) {
         for (int k = 0; k < npasses; k++) {
            glBindFramebuffer(GL_FRAMEBUFFER, small_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (strcmp(mode,"alt")?small_tex:(k&1)?alt_tex:small_tex), 0);
            glViewport(0, 0, psize, psize);
            if (!getenv("NOCLEAR"))
               glClear(GL_COLOR_BUFFER_BIT);
            if (strcmp(mode,"clearonly")) glDrawArrays(GL_TRIANGLES, 0, 3);
         }
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glViewport(0, 0, W, H);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      } else if (!strcmp(mode,"blend") || !strcmp(mode,"blenddepth")) {
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         if (!strcmp(mode,"blenddepth")) glEnable(GL_DEPTH_TEST);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glViewport(0, 0, W, H);
         glClear(GL_COLOR_BUFFER_BIT);
         for (int k = 0; k < npasses; k++)
            glDrawArrays(GL_TRIANGLES, 0, 3);
         glDisable(GL_BLEND);
         glDisable(GL_DEPTH_TEST);
      } else if (!strcmp(mode,"mipgen")) {
         /* Mip-generation cost: regenerate the 512x512 chain every frame, then draw it. */
         glActiveTexture(GL_TEXTURE0);
         glBindTexture(GL_TEXTURE_2D, ctex);
         glGenerateMipmap(GL_TEXTURE_2D);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glViewport(0, 0, W, H);
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      } else if (!strcmp(mode,"manydraws")) {
         for (int k = 0; k < npasses; k++)
            glDrawArrays(GL_TRIANGLES, 0, 3);
      } else {
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, verts ? verts : 3);
      }
      if (!getenv("NOFINISH")) glFinish();
   }
   glFinish();
   double dt=now()-t0;
   struct rusage ru; getrusage(RUSAGE_SELF,&ru);
   double cpu=(ru.ru_utime.tv_sec+ru.ru_utime.tv_usec/1e6)+(ru.ru_stime.tv_sec+ru.ru_stime.tv_usec/1e6);
   printf("mode=%-8s frames=%d fps=%.1f wall=%.1fms cpu=%.1fms verts=%d texture=%s\n", mode, frames,
          frames/dt, dt*1000, cpu*1000, verts,
          strstr(mode,"tex")||strstr(mode,"mip")||strstr(mode,"cube") ? "yes" : "no");
   return 0;
}
