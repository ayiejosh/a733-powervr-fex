/* fp16cmp - is the shaderFloat16 failure a miscompile or just mediump precision?
 *
 * Renders the same mediump fragment shader, reads the framebuffer back, and prints the
 * raw pixels. Run with PVRSRV_FP16=0 and =1 (and MESA_SHADER_CACHE_DISABLE=true) and compare:
 *   a few LSB of difference -> precision (feature is CORRECT, tolerance is the issue)
 *   garbage/inverted/NaN    -> genuine miscompile
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static const char *VS =
  "attribute vec2 p;\n"
  "void main(){ gl_Position=vec4(p,0.0,1.0); }\n";

/* mediump maths in a loop: the pattern the failing glmark2 scenes share. */
static const char *FS_HI =
  "precision highp float;\n"
  "uniform float u;\n"
  "void main(){\n"
  "  highp float x = u;\n"
  "  for (int i=0;i<8;i++) x = x*1.1 + 0.1;\n"
  "  gl_FragColor = vec4(x, x*0.5, x*0.25, 1.0);\n"
  "}\n";

static const char *FS =
  "precision mediump float;\n"
  "uniform float u;\n"
  "void main(){\n"
  "  mediump float x = u;\n"
  "  for (int i=0;i<8;i++) x = x*1.1 + 0.1;\n"
  "  gl_FragColor = vec4(x, x*0.5, x*0.25, 1.0);\n"
  "}\n";

static GLuint mk(GLenum t, const char *s){
  GLuint o=glCreateShader(t); glShaderSource(o,1,&s,NULL); glCompileShader(o);
  GLint ok=0; glGetShaderiv(o,GL_COMPILE_STATUS,&ok);
  if(!ok){ char l[2048]; glGetShaderInfoLog(o,2048,0,l); fprintf(stderr,"compile: %s\n",l); exit(1);} return o;
}

int main(void){
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if(d==EGL_NO_DISPLAY){ fprintf(stderr,"no EGL display\n"); return 2; }
  EGLint maj,min; if(!eglInitialize(d,&maj,&min)){ fprintf(stderr,"eglInitialize failed\n"); return 2; }
  printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER));
  printf("GL_VERSION  : %s\n", glGetString(GL_VERSION));

  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,
               EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
  EGLConfig cfg; EGLint nc;
  if(!eglChooseConfig(d,ca,&cfg,1,&nc)||nc<1){ fprintf(stderr,"no EGL config\n"); return 2; }
  EGLint pb[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,cfg,pb);
  if(s==EGL_NO_SURFACE){ fprintf(stderr,"no pbuffer\n"); return 2; }
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ctxa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ctxa);
  if(c==EGL_NO_CONTEXT){ fprintf(stderr,"no context\n"); return 2; }
  if(!eglMakeCurrent(d,s,s,c)){ fprintf(stderr,"makeCurrent failed 0x%x\n",eglGetError()); return 2; }
  /* re-query now that a context is current */
  printf("RENDERER2   : %s\n", glGetString(GL_RENDERER));

  GLuint p=glCreateProgram();
  glAttachShader(p,mk(GL_VERTEX_SHADER,VS));
  glAttachShader(p,mk(GL_FRAGMENT_SHADER, getenv("USE_HIGHP") ? FS_HI : FS));
  glBindAttribLocation(p,0,"p"); glLinkProgram(p);
  GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok){ char l[2048]; glGetProgramInfoLog(p,2048,0,l); fprintf(stderr,"link: %s\n",l); return 1; }

  /* probe several inputs so a single lucky value cannot mask a real difference */
  const float us[]={0.1f,0.3f,0.7f,1.0f,2.5f,7.5f};
  glViewport(0,0,64,64);
  glUseProgram(p);
  GLint loc=glGetUniformLocation(p,"u");
  unsigned long long sum=0; int bad=0;
  printf("input      R    G    B\n");
  for(unsigned k=0;k<sizeof(us)/sizeof(us[0]);k++){
    glUniform1f(loc,us[k]);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    const GLfloat v[]={-1,-1, 3,-1, -1,3};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,v);
    glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();
    unsigned char px[4]={0,0,0,0};
    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    if(px[0]==0&&px[1]==0&&px[2]==0) bad++;
    printf("  u=%-5.2f  %3u  %3u  %3u\n", us[k], px[0], px[1], px[2]);
    sum = sum*1000003ull + px[0];
    sum = sum*1000003ull + px[1];
    sum = sum*1000003ull + px[2];
  }
  printf("CHECKSUM %llu  black_pixels=%d  glGetError=0x%x\n", sum, bad, glGetError());
  return 0;
}
