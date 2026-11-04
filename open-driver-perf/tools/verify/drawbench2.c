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
static GLint g_mvp_loc = -1, g_vl_loc = -1, g_fl_loc = -1;
static int g_vl_val = 5, g_fl_val = 5;

static char *read_src(const char *path)
{
   FILE *f = fopen(path, "rb");
   long n; char *buf;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
   buf = malloc(n + 1);
   if (buf && fread(buf, 1, n, f) == (size_t)n) buf[n] = 0; else { free(buf); buf = NULL; }
   fclose(f);
   return buf;
}

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
   fprintf(stderr,"R=%s\nV=%s\n",(const char*)glGetString(GL_RENDERER),(const char*)glGetString(GL_VERSION));
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
   { GLuint drb; glGenRenderbuffers(1,&drb); glBindRenderbuffer(GL_RENDERBUFFER,drb);
     glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,W,H);
     glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,drb); }
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
   GLuint idx_vbo = 0; int file_nidx = 0;
   int readback_black = 0;
   const int depthmode = !strcmp(mode,"depthfill") ? 1 : !strcmp(mode,"depthover") ? 2 : 0;
   const int filedepth = getenv("DB2_DEPTH") ? 1 : 0;
   const int filecull = getenv("DB2_CULL") ? 1 : 0;
   const char *fs_fill = "precision mediump float; void main(){ gl_FragColor = vec4(0.5,0.25,0.75,1.0); }";
   const char *fs_tex  = "precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }";
   const char *fs_cube = "precision mediump float; varying vec2 v; uniform samplerCube t; void main(){ gl_FragColor = textureCube(t, vec3(v*2.0-1.0, 1.0)); }";
   /* Fragment ALU throughput: a dependent 128-step FMA chain per pixel (highp, so no FP16 path). */
   const char *fs_alu = "precision highp float; varying vec2 v; void main(){ float x = v.x + v.y*0.5; for (int i=0;i<128;i++) { x = x*0.9999 + v.y*0.5; x = x - floor(x); } gl_FragColor = vec4(x, x*0.5, x*0.25, 1.0); }";
   /* Same draw repeated with a changing uniform: the per-draw state block differs every draw, so a
    * driver cannot reuse it (unlike "manydraws"). */
   const char *fs_fillu = "precision mediump float; uniform float u; void main(){ gl_FragColor = vec4(u, 0.25, 0.75, 1.0); }";
   /* A realistic lighting/math fragment shader: normalize, dot, pow, exp2, log2, sqrt, rsqrt. */
   const char *fs_math = "precision highp float; varying vec2 v; void main(){ vec3 n = normalize(vec3(v, 1.0)); vec3 l = normalize(vec3(0.3, 0.5, 0.8)); float d = max(dot(n, l), 0.0); float s = pow(d, 16.0); float e = exp2(v.x) * log2(1.0 + abs(v.y)); float q = sqrt(abs(v.x)) + inversesqrt(1.0 + abs(v.y)); gl_FragColor = vec4(s, e, q, 1.0); }";
   const char *fs_mathm = "precision mediump float; varying vec2 v; void main(){ vec3 n = normalize(vec3(v, 1.0)); vec3 l = normalize(vec3(0.3, 0.5, 0.8)); float d = max(dot(n, l), 0.0); float s = pow(d, 16.0); float e = exp2(v.x) * log2(1.0 + abs(v.y)); float q = sqrt(abs(v.x)) + inversesqrt(1.0 + abs(v.y)); gl_FragColor = vec4(s, e, q, 1.0); }";
   /* ILP/issue-bound: 64 independent FMULs, summed at the end. */
   /* Same but with varying multipliers (no per-iteration literals: no immediate materialisation). */
   const char *fs_ilp = "precision highp float; varying vec2 v; void main(){ float a0=v.x,a1=v.y,a2=1.0-v.x,a3=1.0-v.y; float s=0.0; for(int i=0;i<16;i++){ a0=a0*v.x; a1=a1*v.y; a2=a2*v.x; a3=a3*v.y; s+=a0+a1+a2+a3; } gl_FragColor=vec4(s,a0,a1,a2); }";
   /* Special-function throughput: only frsq/frcp/fexp/flog chains. */
   const char *fs_trans = "precision highp float; varying vec2 v; void main(){ float x=v.x+1.5, y=v.y+1.5; for(int i=0;i<8;i++){ x=sqrt(x); y=inversesqrt(y); x=exp2(x); y=log2(y); } gl_FragColor=vec4(x,y,x*y,1.0); }";
   GLuint cube=0;
   const char *vs_file = getenv("VS_FILE"), *fs_file = getenv("FS_FILE");
   if (vs_file && fs_file) {
      char *vsrc = read_src(vs_file), *fsrc = read_src(fs_file);
      if (!vsrc || !fsrc) { fprintf(stderr, "cannot read shader files\n"); return 1; }
      p = prog(vsrc, fsrc);
   } else if (!strcmp(mode,"fill")||!strcmp(mode,"overdraw")||depthmode) p = prog(VS_FULL, fs_fill);
   else if (!strcmp(mode,"cube")) {
      glGenTextures(1,&cube); glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
      for(int f=0;f<6;f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
      glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      p = prog(VS_FULL, fs_cube);
   } else if (!strcmp(mode,"alu")) { p = prog(VS_FULL, fs_alu); }
   else if (!strcmp(mode,"manydrawsflex")) p = prog(VS_FULL, fs_fillu);
   else if (!strcmp(mode,"math")) p = prog(VS_FULL, fs_math);
   else if (!strcmp(mode,"mathm")) p = prog(VS_FULL, fs_mathm);
   else if (!strcmp(mode,"ilp")) p = prog(VS_FULL, fs_ilp);
   else if (!strcmp(mode,"trans")) p = prog(VS_FULL, fs_trans);
   else if (!strcmp(mode,"vert")) { p = prog(VS_GRID, fs_fill); verts = nverts; }
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
   if (vs_file && fs_file) {
      GLint natt = 0;
      glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &natt);
      int file_verts = getenv("FILE_VERTS") ? atoi(getenv("FILE_VERTS")) : 0;
   GLuint vbo = 0;
   if (file_verts > 0) {
      float *pbuf = malloc((size_t)file_verts * 3 * sizeof(float));
      if (pbuf) {
         for (int i = 0; i < file_verts; i++) {
            pbuf[i * 3 + 0] = (float)((i * 37) % 1000) / 500.0f - 1.0f;
            pbuf[i * 3 + 1] = (float)((i * 91) % 1000) / 500.0f - 1.0f;
            pbuf[i * 3 + 2] = (float)((i * 13) % 100) / 200.0f;
         }
         glGenBuffers(1, &vbo);
         glBindBuffer(GL_ARRAY_BUFFER, vbo);
         glBufferData(GL_ARRAY_BUFFER, (size_t)file_verts * 3 * sizeof(float), pbuf, GL_STATIC_DRAW);
         free(pbuf);
      }
      verts = file_verts;
      if (getenv("FILE_INDEXED")) {
         if (file_verts > 65535) {
            fprintf(stderr, "FILE_VERTS capped to 65535 for 16-bit indices\n");
            file_verts = verts = 65535;
         }
         const int nidx = (file_verts / 3) * 3;
         unsigned short *ibuf = malloc((size_t)nidx * sizeof(unsigned short));
         for (int i = 0; i < nidx; i++) ibuf[i] = (unsigned short)((i * 7 + (i / 3)) % file_verts);
         glGenBuffers(1, &idx_vbo);
         glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, idx_vbo);
         glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)nidx * sizeof(unsigned short), ibuf, GL_STATIC_DRAW);
         free(ibuf);
         file_nidx = nidx;
      }
   }
   for (GLint i = 0; i < natt; i++) {
         char name[128]; GLint sz = 0; GLenum ty = 0;
         glGetActiveAttrib(p, (GLuint)i, sizeof(name), NULL, &sz, &ty, name);
         GLint loc = glGetAttribLocation(p, name);
         if (loc >= 0) {
            glVertexAttribPointer((GLuint)loc, file_verts > 0 ? 3 : 2, GL_FLOAT, GL_FALSE, 0,
                                  file_verts > 0 ? (const void *)0 : (const void *)quad);
            glEnableVertexAttribArray((GLuint)loc);
         }
      }
   }
   glUseProgram(p);
   if (getenv("VS_FILE")) {
      /* The composed scene vertex shaders transform with a mat4 uniform; set every mat4 to identity
       * so the vertices spread over the target instead of collapsing to the origin (which would
       * skip all fragment work). */
      GLint nu = 0, maxlen = 0;
      glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
      glGetProgramiv(p, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxlen);
      char *un = malloc((size_t)maxlen + 1);
      for (GLint i = 0; i < nu && un; i++) {
         GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
         glGetActiveUniform(p, (GLuint)i, maxlen + 1, &len, &sz, &ty, un);
         if (ty == GL_FLOAT_MAT4) {
            GLint loc = glGetUniformLocation(p, un);
            const GLfloat ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            if (loc >= 0) { glUniformMatrix4fv(loc, 1, GL_FALSE, ident); g_mvp_loc = loc; }
         }
      }
      free(un);
   }
   const char *set_int = getenv("DB2_SET_INT");
   if (set_int) {
      const char *c = set_int;
      while (*c) {
         char name[64]; int val = 0, used = 0;
         if (sscanf(c, "%63[^=]=%d%n", name, &val, &used) >= 2) {
            GLint loc = glGetUniformLocation(p, name);
            if (loc >= 0) glUniform1i(loc, val);
            else fprintf(stderr, "DB2_SET_INT: unknown uniform '%s' ignored\n", name);
            if (!strcmp(name, "VertexLoops")) { g_vl_loc = loc; g_vl_val = val; }
            if (!strcmp(name, "FragmentLoops")) { g_fl_loc = loc; g_fl_val = val; }
            c += used;
            if (*c == ',') c++;
         } else {
            fprintf(stderr, "DB2_SET_INT: cannot parse '%s'\n", c);
            break;
         }
      }
   }
   GLint flex_u = glGetUniformLocation(p, "u");
   if (cube) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_CUBE_MAP,cube); glUniform1i(glGetUniformLocation(p,"t"),0); }
   else if (!verts && strcmp(mode,"fill") && strcmp(mode,"overdraw")) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,ctex); glUniform1i(glGetUniformLocation(p,"t"),0); }
   if (depthmode) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearDepthf(1.0f); }
   if (filedepth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glClearDepthf(1.0f); }
   if (filecull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); }
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
      } else if (!strcmp(mode,"manydrawsflex")) {
         for (int k = 0; k < npasses; k++) {
            glUniform1f(flex_u, (float)k * 0.001f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
         }
      } else {
         if (getenv("DB2_UNIFORM_PER_FRAME")) {
            GLfloat m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0.01f*(float)(f & 1),0,0,1 };
            if (g_mvp_loc >= 0) glUniformMatrix4fv(g_mvp_loc, 1, GL_FALSE, m);
            if (g_vl_loc >= 0) glUniform1i(g_vl_loc, g_vl_val);
            if (g_fl_loc >= 0) glUniform1i(g_fl_loc, g_fl_val);
         }
         glClear(GL_COLOR_BUFFER_BIT | ((depthmode || filedepth) ? GL_DEPTH_BUFFER_BIT : 0));
         if (depthmode == 2) {
            /* Same quad, same depth, 27 times: early-Z must reject all but the first. */
            for (int k = 0; k < npasses; k++)
               glDrawArrays(GL_TRIANGLES, 0, 3);
         } else {
            if (file_nidx > 0)
               glDrawElements(GL_TRIANGLES, file_nidx, GL_UNSIGNED_SHORT, (const void *)0);
            else
               glDrawArrays(GL_TRIANGLES, 0, verts ? verts : 3);
         }
      }
      if (!getenv("NOFINISH")) glFinish();
   }
   if (getenv("DB2_READBACK")) {
      unsigned char px[4] = { 0, 0, 0, 0 };
      glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      fprintf(stderr, "readback centre pixel: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
      if (px[0] == 0 && px[1] == 0 && px[2] == 0) {
         fprintf(stderr, "readback centre pixel is black - no fragments reached the target\n");
         readback_black = 1;
      }
   }
   glFinish();
   double dt=now()-t0;
   struct rusage ru; getrusage(RUSAGE_SELF,&ru);
   double cpu=(ru.ru_utime.tv_sec+ru.ru_utime.tv_usec/1e6)+(ru.ru_stime.tv_sec+ru.ru_stime.tv_usec/1e6);
   printf("mode=%-8s frames=%d fps=%.1f wall=%.1fms cpu=%.1fms verts=%d texture=%s\n", mode, frames,
          frames/dt, dt*1000, cpu*1000, verts,
          strstr(mode,"tex")||strstr(mode,"mip")||strstr(mode,"cube") ? "yes" : "no");
   return readback_black ? 3 : 0;
}
