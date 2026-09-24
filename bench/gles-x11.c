/* Windowed GLES2 on the X display — the "can the GPU actually show a window?" test.
 *
 * glbench.c measures GPU throughput off-screen (GBM/FBO). This measures the thing the
 * desktop care about: a real X11 window, a real EGL window surface, a real
 * eglSwapBuffers() every frame, i.e. GPU render -> dma-buf -> X present -> HDMI scanout.
 *
 * Run it against the vendor PowerVR stack and against llvmpipe on the SAME display and
 * you get the windowed A/B:
 *
 *   LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gles-x11 800 600 64 900   # PowerVR
 *   LIBGL_ALWAYS_SOFTWARE=1     DISPLAY=:0 ./gles-x11 800 600 64 900      # llvmpipe
 *
 * Prints the EGL/GL identity, frames/s, Mpix/s and swap errors. Exits by itself.
 * If this hangs the board, the windowed-GPU-present path is the kernel deadlock class.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

static const char *VS =
"attribute vec2 p; void main(){ gl_Position=vec4(p,0.0,1.0); }";
static const char *FS_TMPL =
"precision highp float; uniform float t;\n"
"void main(){ vec2 c=gl_FragCoord.xy*0.001; float a=t;\n"
" for(int i=0;i<%d;i++){ a=a*1.0001+sin(c.x+a)*cos(c.y-a)+fract(a*1.3); c+=vec2(a,-a)*0.0001; }\n"
" gl_FragColor=vec4(fract(a),c.x,c.y,1.0); }";

static GLuint sh(GLenum k,const char*s){
    GLuint o=glCreateShader(k); glShaderSource(o,1,&s,0); glCompileShader(o);
    GLint ok; glGetShaderiv(o,GL_COMPILE_STATUS,&ok);
    if(!ok){ char l[2048]; glGetShaderInfoLog(o,2048,0,l); fprintf(stderr,"shader: %s\n",l); }
    return o;
}

int main(int argc,char**argv){
    int W=argc>1?atoi(argv[1]):800, H=argc>2?atoi(argv[2]):600;
    int iters=argc>3?atoi(argv[3]):64, frames=argc>4?atoi(argv[4]):900;
    char fs[1024]; snprintf(fs,sizeof fs,FS_TMPL,iters);

    Display *xdpy = XOpenDisplay(NULL);
    if(!xdpy){ fprintf(stderr,"cannot open X display '%s'\n", getenv("DISPLAY")?getenv("DISPLAY"):"(unset)"); return 2; }
    int scr = DefaultScreen(xdpy);
    Window win = XCreateSimpleWindow(xdpy,RootWindow(xdpy,scr),0,0,W,H,0,
                                     BlackPixel(xdpy,scr),BlackPixel(xdpy,scr));
    XStoreName(xdpy,win,"gles-x11 (PowerVR windowed test)");
    XSelectInput(xdpy,win,ExposureMask|StructureNotifyMask);
    XMapWindow(xdpy,win);
    XSync(xdpy,False);

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
    if(dpy==EGL_NO_DISPLAY){ fprintf(stderr,"no EGL display (eglGetError 0x%x)\n",eglGetError()); return 1; }
    EGLint maj,min;
    if(!eglInitialize(dpy,&maj,&min)){ fprintf(stderr,"eglInitialize failed 0x%x\n",eglGetError()); return 1; }
    printf("EGL_VERSION : %d.%d\n",maj,min);
    printf("EGL_VENDOR  : %s\n", eglQueryString(dpy,EGL_VENDOR));
    printf("EGL_CLIENT_APIS: %s\n", eglQueryString(dpy,EGL_CLIENT_APIS));
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint ca[]={ EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
                  EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE };
    EGLConfig cfg; EGLint nc=0;
    if(!eglChooseConfig(dpy,ca,&cfg,1,&nc)||nc<1){ fprintf(stderr,"no EGL config 0x%x\n",eglGetError()); return 1; }
    EGLint cta[]={ EGL_CONTEXT_CLIENT_VERSION,2, EGL_NONE };
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,cta);
    if(ctx==EGL_NO_CONTEXT){ fprintf(stderr,"no EGL context 0x%x\n",eglGetError()); return 1; }
    EGLSurface surf=eglCreateWindowSurface(dpy,cfg,(EGLNativeWindowType)win,NULL);
    if(surf==EGL_NO_SURFACE){ fprintf(stderr,"no EGL window surface 0x%x\n",eglGetError()); return 1; }
    if(!eglMakeCurrent(dpy,surf,surf,ctx)){ fprintf(stderr,"eglMakeCurrent failed 0x%x\n",eglGetError()); return 1; }
    /* SWAP_INTERVAL=0 -> uncapped throughput; default 1 -> real presentation (vblank-locked) */
    int si = getenv("SWAP_INTERVAL") ? atoi(getenv("SWAP_INTERVAL")) : 1;
    eglSwapInterval(dpy, si);

    printf("GL_VENDOR   : %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION  : %s\n", glGetString(GL_VERSION));
    printf("surface     : %dx%d %s\n", W,H, eglQueryString(dpy,EGL_EXTENSIONS) ? "" : "");
    printf("window      : %dx%d, %d frames, shader loop=%d\n", W,H,frames,iters);
    fflush(stdout);

    GLuint p=glCreateProgram();
    glAttachShader(p,sh(GL_VERTEX_SHADER,VS));
    glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
    glBindAttribLocation(p,0,"p");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char l[2048]; glGetProgramInfoLog(p,2048,0,l); fprintf(stderr,"link: %s\n",l); return 1; }
    glUseProgram(p);
    GLint uloc=glGetUniformLocation(p,"t");
    /* fullscreen triangle strip */
    GLfloat verts[8]={-1,-1, 1,-1, -1,1, 1,1};
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,verts);
    glEnableVertexAttribArray(0);
    glViewport(0,0,W,H);

    /* BLIT=1: render the heavy shader into a GPU-local FBO, then copy it to the window.
       Composites the way a compositor does and avoids shading directly into the
       (apparently slow) presentation buffer. */
    int blit = getenv("BLIT") && atoi(getenv("BLIT"));
    GLuint fbo=0, tex=0, pcopy=0, upcopy=0;
    if(blit){
        glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){
            fprintf(stderr,"BLIT: FBO incomplete\n"); return 1; }
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        const char *VS2="attribute vec2 p; varying vec2 uv; void main(){ uv=p*0.5+0.5; gl_Position=vec4(p,0.0,1.0); }";
        const char *FS2="precision mediump float; varying vec2 uv; uniform sampler2D s;"
                        "void main(){ gl_FragColor=texture2D(s,uv); }";
        pcopy=glCreateProgram();
        glAttachShader(pcopy,sh(GL_VERTEX_SHADER,VS2));
        glAttachShader(pcopy,sh(GL_FRAGMENT_SHADER,FS2));
        glBindAttribLocation(pcopy,0,"p");
        glLinkProgram(pcopy);
        glGetProgramiv(pcopy,GL_LINK_STATUS,&ok);
        if(!ok){ char l[2048]; glGetProgramInfoLog(pcopy,2048,0,l); fprintf(stderr,"copy link: %s\n",l); return 1; }
        upcopy=glGetUniformLocation(pcopy,"s");
        printf("mode        : BLIT (render offscreen -> copy into the window surface)\n");
    } else {
        printf("mode        : direct (shade straight into the window surface)\n");
    }
    fflush(stdout);

    int swerr=0;
    double t0=now(), tdraw=0, tfin=0, tswap=0;
    for(int i=0;i<frames;i++){
        double a=now();
        if(blit){
            glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glUseProgram(p); glUniform1f(uloc,(float)i*0.01f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            glUseProgram(pcopy); glUniform1i(upcopy,0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        } else {
            glUseProgram(p); glUniform1f(uloc,(float)i*0.01f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        }
        double b=now();
        glFinish();
        double c=now();
        if(!eglSwapBuffers(dpy,surf)) swerr++;
        double d=now();
        tdraw+=b-a; tfin+=c-b; tswap+=d-c;
    }
    double dt=now()-t0;

    printf("RESULT      : %.1f fps  %.1f Mpix/s  (%.3f s, %d swap errors)\n",
           frames/dt, frames*(double)W*H/dt/1e6, dt, swerr);
    printf("PHASES      : draw %.2f | glFinish %.2f | eglSwapBuffers %.2f  ms/frame\n",
           1000.0*tdraw/frames, 1000.0*tfin/frames, 1000.0*tswap/frames);
    fflush(stdout);

    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroySurface(dpy,surf);
    eglDestroyContext(dpy,ctx);
    eglTerminate(dpy);
    XDestroyWindow(xdpy,win);
    XCloseDisplay(xdpy);
    return swerr?1:0;
}
