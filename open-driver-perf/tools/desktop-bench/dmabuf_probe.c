/*
 * dmabuf_probe [W H] - the Firefox WebGL buffer path without Firefox: a linear ARGB8888 GBM buffer on
 * the render node, imported into GLES as an EGLImage, cleared blue with the top-right quadrant (GL
 * coordinates) red, then read back (a) through the CPU (gbm_bo_map) and (b) through a second import
 * read with glReadPixels. Prints the colour at the centre of each quadrant; red must be top-right only.
 * Build: cc -O2 -o dmabuf_probe dmabuf_probe.c -lgbm -lEGL -lGLESv2
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

static const char *colour(uint32_t argb)
{
   int r = (argb >> 16) & 0xff, b = argb & 0xff;
   return r > 200 && b < 50 ? "red" : b > 200 && r < 50 ? "blue" : "OTHER";
}

/* EGLImage for the buffer, attached to a texture and a framebuffer */
static GLuint import_fbo(EGLDisplay dpy, struct gbm_bo *bo, int w, int h)
{
   int fd = gbm_bo_get_fd(bo);
   uint64_t mod = gbm_bo_get_modifier(bo);
   EGLint attr[] = {
      EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)gbm_bo_get_offset(bo, 0),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE };
   EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
   close(fd);
   if (img == EGL_NO_IMAGE_KHR) { printf("eglCreateImage FAILED 0x%x\n", eglGetError()); exit(1); }
   GLuint tex, fbo;
   glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
   image_target_texture(GL_TEXTURE_2D, img);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (st != GL_FRAMEBUFFER_COMPLETE) { printf("framebuffer incomplete 0x%x\n", st); exit(1); }
   return fbo;
}

int main(int argc, char **argv)
{
   int w = argc > 2 ? atoi(argv[1]) : 2844, h = argc > 2 ? atoi(argv[2]) : 1600;
   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   struct gbm_device *gbm = gbm_create_device(fd);
   uint64_t linear = DRM_FORMAT_MOD_LINEAR;
   struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, w, h, GBM_FORMAT_ARGB8888, &linear, 1,
                                                     GBM_BO_USE_RENDERING);
   if (!bo) { printf("gbm_bo_create_with_modifiers FAILED\n"); return 1; }
   printf("bo %dx%d: backend %s stride %u (expect %d) offset %u modifier 0x%llx\n", w, h,
          gbm_device_get_backend_name(gbm), gbm_bo_get_stride(bo), w * 4, gbm_bo_get_offset(bo, 0),
          (unsigned long long)gbm_bo_get_modifier(bo));

   EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   eglInitialize(dpy, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
   create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
   image_target_texture = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   printf("GL: %s\n", glGetString(GL_RENDERER));

   import_fbo(dpy, bo, w, h);
   glViewport(0, 0, w, h);
   glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_SCISSOR_TEST); glScissor(w / 2, h / 2, w - w / 2, h - h / 2);
   glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST);
   glFinish();

   /* quadrant centres in GL coordinates: bottom-left, bottom-right, top-left, top-right */
   int qx[4] = { w / 4, 3 * w / 4, w / 4, 3 * w / 4 }, qy[4] = { h / 4, h / 4, 3 * h / 4, 3 * h / 4 };
   const char *qn[4] = { "bottom-left", "bottom-right", "top-left", "top-right" };
   int bad = 0;

   uint32_t mstride; void *map_data = NULL;
   uint8_t *m = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &mstride, &map_data);
   printf("CPU map (stride %u), memory row = GL y:\n", mstride);
   for (int i = 0; i < 4; i++) {
      uint32_t px = *(uint32_t *)(m + (size_t)qy[i] * mstride + qx[i] * 4);
      printf("  %-12s 0x%08x %s\n", qn[i], px, colour(px));
   }
   gbm_bo_unmap(bo, map_data);

   import_fbo(dpy, bo, w, h);
   printf("second import, glReadPixels:\n");
   for (int i = 0; i < 4; i++) {
      uint8_t p[4];
      glReadPixels(qx[i], qy[i], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
      uint32_t argb = 0xff000000u | p[0] << 16 | p[1] << 8 | p[2];
      const char *c = colour(argb);
      printf("  %-12s 0x%08x %s\n", qn[i], argb, c);
      if (strcmp(c, i == 3 ? "red" : "blue")) bad++;
   }
   printf("%s\n", bad ? "MISMATCH" : "import round trip ok");

   /* (c) what a compositor does: sample the imported buffer as a texture with a full-screen quad
    * into an ordinary w x h texture, then read the quadrants of that copy */
   static const char *vs = "attribute vec2 p; varying vec2 uv; void main(){ uv=p*.5+.5; gl_Position=vec4(p,0.,1.); }";
   static const char *fs = "precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){ gl_FragColor=texture2D(t,uv); }";
   GLuint prog = glCreateProgram();
   const char *src[2] = { vs, fs }; GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(type[i]); glShaderSource(s, 1, &src[i], NULL); glCompileShader(s);
      glAttachShader(prog, s);
   }
   glBindAttribLocation(prog, 0, "p"); glLinkProgram(prog); glUseProgram(prog);
   static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad); glEnableVertexAttribArray(0);
   /* the second import's texture is still bound to unit 0 */
   GLuint dst, dfbo;
   glGenTextures(1, &dst); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, dst);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glActiveTexture(GL_TEXTURE0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glGenFramebuffers(1, &dfbo); glBindFramebuffer(GL_FRAMEBUFFER, dfbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
   glUniform1i(glGetUniformLocation(prog, "t"), 0);
   glViewport(0, 0, w, h); glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glFinish();
   int bad2 = 0;
   printf("sampled as a texture (full-screen quad):\n");
   for (int i = 0; i < 4; i++) {
      uint8_t p[4];
      glReadPixels(qx[i], qy[i], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
      uint32_t argb = 0xff000000u | p[0] << 16 | p[1] << 8 | p[2];
      const char *c = colour(argb);
      printf("  %-12s 0x%08x %s\n", qn[i], argb, c);
      if (strcmp(c, i == 3 ? "red" : "blue")) bad2++;
   }
   printf("%s\n", bad2 ? "SAMPLING MISMATCH" : "sampling ok");

   /* (d) textureSize() as WebRender uses it to normalise texel coordinates: a GLES 3 context writes
    * the size the shader sees into one pixel (x low/high byte, y low/high byte) */
   EGLint c3[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   EGLContext ctx3 = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, c3);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx3);
   static const char *vs3 = "#version 300 es\nin vec2 p; void main(){ gl_Position=vec4(p,0.,1.); }";
   static const char *fs3 = "#version 300 es\nprecision highp float; uniform highp sampler2D t; out vec4 o;\n"
      "void main(){ ivec2 s=textureSize(t,0); o=vec4(float(s.x%256),float(s.x/256),float(s.y%256),float(s.y/256))/255.; }";
   GLuint p3 = glCreateProgram();
   const char *src3[2] = { vs3, fs3 };
   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(type[i]); glShaderSource(s, 1, &src3[i], NULL); glCompileShader(s);
      GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok); if (!ok) { printf("shader %d failed\n", i); return 1; }
      glAttachShader(p3, s);
   }
   glBindAttribLocation(p3, 0, "p"); glLinkProgram(p3); glUseProgram(p3);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad); glEnableVertexAttribArray(0);
   GLuint out, ofbo;
   glGenTextures(1, &out); glBindTexture(GL_TEXTURE_2D, out);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &ofbo); glBindFramebuffer(GL_FRAMEBUFFER, ofbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
   int bad3 = 0;
   for (int imported = 1; imported >= 0; imported--) {
      GLuint t;
      glGenTextures(1, &t); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, t);
      if (imported) {
         int bfd = gbm_bo_get_fd(bo);
         uint64_t mod = gbm_bo_get_modifier(bo);
         EGLint a[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, bfd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE };
         EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
         close(bfd);
         image_target_texture(GL_TEXTURE_2D, img);
      } else {
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glUniform1i(glGetUniformLocation(p3, "t"), 1);
      glViewport(0, 0, 4, 4); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      uint8_t px[4]; glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      int sx = px[0] | px[1] << 8, sy = px[2] | px[3] << 8;
      printf("textureSize() of %s texture: %dx%d (real %dx%d)%s\n", imported ? "imported" : "ordinary",
             sx, sy, w, h, sx == w && sy == h ? "" : "  WRONG");
      bad3 += sx != w || sy != h;
   }
   return bad + bad2 + bad3 != 0;
}
