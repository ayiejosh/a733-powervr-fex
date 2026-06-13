/* Headless GLES2 fill-rate / shader benchmark.
   Same binary runs on the PowerVR GPU (LD_LIBRARY_PATH=/usr/local/lib + a DRM render
   node arg) and on llvmpipe (system libs, surfaceless) -> apples-to-apples.
   Renders a fullscreen quad with an ALU-loop fragment shader to a WxH FBO, many times,
   reports frames/sec and effective Mpixels/sec. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

static const char *VS =
"attribute vec2 p; void main(){ gl_Position=vec4(p,0.0,1.0); }";
/* ALU-heavy fragment shader: LOOP iterations of transcendental+fma per pixel */
static const char *FS_TMPL =
"precision highp float; uniform float t;\n"
"void main(){ vec2 c=gl_FragCoord.xy*0.001; float a=t;\n"
" for(int i=0;i<%d;i++){ a=a*1.0001+sin(c.x+a)*cos(c.y-a)+fract(a*1.3); c+=vec2(a,-a)*0.0001; }\n"
" gl_FragColor=vec4(fract(a),c.x,c.y,1.0); }";

static GLuint sh(GLenum k,const char*s){GLuint o=glCreateShader(k);glShaderSource(o,1,&s,0);glCompileShader(o);
 GLint ok;glGetShaderiv(o,GL_COMPILE_STATUS,&ok); if(!ok){char l[2048];glGetShaderInfoLog(o,2048,0,l);fprintf(stderr,"shader: %s\n",l);} return o;}

int main(int argc,char**argv){
    const char *dev = (argc>1 && strcmp(argv[1],"none"))?argv[1]:NULL;     /* DRM render node for GBM; NULL=surfaceless */
    int W=1280,H=720, iters=argc>2?atoi(argv[2]):64, frames=argc>3?atoi(argv[3]):300;
    EGLDisplay dpy=EGL_NO_DISPLAY; struct gbm_device*gbm=NULL; int fd=-1;
    if(dev && !strcmp(dev,"x11")){ dpy=eglGetDisplay(EGL_DEFAULT_DISPLAY); /* uses $DISPLAY (e.g. :1 llvmpipe) */
    } else if(dev){ fd=open(dev,O_RDWR); if(fd<0){perror("open drm");return 1;} gbm=gbm_create_device(fd);
        PFNEGLGETPLATFORMDISPLAYEXTPROC getPD=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
        dpy=getPD(EGL_PLATFORM_GBM_KHR,gbm,NULL);
    } else {
        PFNEGLGETPLATFORMDISPLAYEXTPROC getPD=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
        dpy=getPD(0x31DD/*EGL_PLATFORM_SURFACELESS_MESA*/,EGL_DEFAULT_DISPLAY,NULL);
    }
    if(dpy==EGL_NO_DISPLAY){fprintf(stderr,"no EGL display\n");return 1;}
    EGLint maj,min; if(!eglInitialize(dpy,&maj,&min)){fprintf(stderr,"eglInitialize fail 0x%x\n",eglGetError());return 1;}
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
    EGLConfig cfg; EGLint nc; eglChooseConfig(dpy,ca,&cfg,1,&nc);
    EGLint cta[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,cta);
    if(ctx==EGL_NO_CONTEXT){fprintf(stderr,"no context 0x%x\n",eglGetError());return 1;}
    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx);
    printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER));
    printf("GL_VENDOR   : %s\n", glGetString(GL_VENDOR));
    printf("GL_VERSION  : %s\n", glGetString(GL_VERSION));

    /* offscreen FBO */
    GLuint tex,fbo; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"FBO incomplete\n");return 1;}
    glViewport(0,0,W,H);

    char fs[1024]; snprintf(fs,sizeof fs,FS_TMPL,iters);
    GLuint prog=glCreateProgram(); glAttachShader(prog,sh(GL_VERTEX_SHADER,VS)); glAttachShader(prog,sh(GL_FRAGMENT_SHADER,fs));
    glBindAttribLocation(prog,0,"p"); glLinkProgram(prog); glUseProgram(prog);
    GLint ut=glGetUniformLocation(prog,"t");
    float quad[]={-1,-1, 3,-1, -1,3};   /* big triangle covers screen */
    GLuint vb; glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY_BUFFER,vb);
    glBufferData(GL_ARRAY_BUFFER,sizeof quad,quad,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);

    /* warmup */
    for(int i=0;i<10;i++){glUniform1f(ut,(float)i);glDrawArrays(GL_TRIANGLES,0,3);} glFinish();
    double t0=now();
    for(int i=0;i<frames;i++){ glUniform1f(ut,(float)i); glDrawArrays(GL_TRIANGLES,0,3); }
    glFinish();
    double dt=now()-t0;
    double fps=frames/dt, mpix=fps*W*H/1e6;
    printf("RESULT: %dx%d  shaderLoop=%d  %d frames in %.3fs -> %.1f fps, %.0f Mpix/s\n",
           W,H,iters,frames,dt,fps,mpix);
    return 0;
}
