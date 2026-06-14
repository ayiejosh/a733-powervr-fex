// Local-only EGL host thunk with 64<->32 opaque-handle mapping (NOT a FEX contribution).
// Replicates ThunkLibs/libEGL/libEGL_Host.cpp and adds custom impls that map
// EGLConfig/EGLContext/EGLSurface to 32-bit tokens for i386 guests; passthrough for x86-64.
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>

#include <EGL/egl.h>

#include "common/Host.h"

#ifdef IS_32BIT_THUNK
#include <vector>
#include <mutex>
// global 64<->32 handle table: token (1-based index) <-> host handle pointer
static std::vector<void*>& g_handles() { static std::vector<void*> v; return v; }
static std::mutex g_hmtx;
static uint32_t map_handle(void* h) {
  if (!h) return 0;
  std::lock_guard<std::mutex> lk(g_hmtx);
  auto& v = g_handles();
  for (size_t i = 0; i < v.size(); ++i) if (v[i] == h) return (uint32_t)(i + 1);
  v.push_back(h);
  return (uint32_t)v.size();
}
static void* unmap_handle(void* token) {
  uint32_t t = (uint32_t)(uintptr_t)token;
  if (!t) return nullptr;
  std::lock_guard<std::mutex> lk(g_hmtx);
  auto& v = g_handles();
  return (t <= v.size()) ? v[t - 1] : nullptr;
}
#define MAPH(h)   ((void*)(uintptr_t)map_handle((void*)(h)))
#define UNMAPH(t) (unmap_handle((void*)(t)))
#else
#define MAPH(h)   (h)
#define UNMAPH(t) (t)
#endif

#include "thunkgen_host_libEGL.inl"

// ---- custom impls (map/unmap handles on 32-bit; identity on 64-bit) ----
static auto fexfn_impl_libEGL_eglChooseConfig(EGLDisplay dpy, const EGLint* al, EGLConfig* configs, EGLint sz, EGLint* num) -> EGLBoolean {
#ifdef IS_32BIT_THUNK
  if (!configs) return eglChooseConfig(dpy, al, nullptr, sz, num);
  std::vector<EGLConfig> hc(sz > 0 ? sz : 1);
  EGLBoolean r = eglChooseConfig(dpy, al, hc.data(), sz, num);
  if (r) { uint32_t* g = (uint32_t*)configs; int k = (num && *num < sz) ? *num : sz; for (int i = 0; i < k; ++i) g[i] = map_handle(hc[i]); }
  return r;
#else
  return eglChooseConfig(dpy, al, configs, sz, num);
#endif
}
static auto fexfn_impl_libEGL_eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint sz, EGLint* num) -> EGLBoolean {
#ifdef IS_32BIT_THUNK
  if (!configs) return eglGetConfigs(dpy, nullptr, sz, num);
  std::vector<EGLConfig> hc(sz > 0 ? sz : 1);
  EGLBoolean r = eglGetConfigs(dpy, hc.data(), sz, num);
  if (r) { uint32_t* g = (uint32_t*)configs; int k = (num && *num < sz) ? *num : sz; for (int i = 0; i < k; ++i) g[i] = map_handle(hc[i]); }
  return r;
#else
  return eglGetConfigs(dpy, configs, sz, num);
#endif
}
static auto fexfn_impl_libEGL_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig c, EGLint a, EGLint* v) -> EGLBoolean {
  return eglGetConfigAttrib(dpy, (EGLConfig)UNMAPH(c), a, v);
}
static auto fexfn_impl_libEGL_eglCreateContext(EGLDisplay dpy, EGLConfig c, EGLContext sh, const EGLint* al) -> EGLContext {
  return (EGLContext)MAPH(eglCreateContext(dpy, (EGLConfig)UNMAPH(c), (EGLContext)UNMAPH(sh), al));
}
static auto fexfn_impl_libEGL_eglDestroyContext(EGLDisplay dpy, EGLContext c) -> EGLBoolean {
  return eglDestroyContext(dpy, (EGLContext)UNMAPH(c));
}
static auto fexfn_impl_libEGL_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig c, const EGLint* al) -> EGLSurface {
  return (EGLSurface)MAPH(eglCreatePbufferSurface(dpy, (EGLConfig)UNMAPH(c), al));
}
static auto fexfn_impl_libEGL_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig c, EGLNativeWindowType w, const EGLint* al) -> EGLSurface {
  return (EGLSurface)MAPH(eglCreateWindowSurface(dpy, (EGLConfig)UNMAPH(c), w, al));
}
static auto fexfn_impl_libEGL_eglDestroySurface(EGLDisplay dpy, EGLSurface s) -> EGLBoolean {
  return eglDestroySurface(dpy, (EGLSurface)UNMAPH(s));
}
static auto fexfn_impl_libEGL_eglMakeCurrent(EGLDisplay dpy, EGLSurface d, EGLSurface r, EGLContext c) -> EGLBoolean {
  return eglMakeCurrent(dpy, (EGLSurface)UNMAPH(d), (EGLSurface)UNMAPH(r), (EGLContext)UNMAPH(c));
}
static auto fexfn_impl_libEGL_eglSwapBuffers(EGLDisplay dpy, EGLSurface s) -> EGLBoolean {
  return eglSwapBuffers(dpy, (EGLSurface)UNMAPH(s));
}
static auto fexfn_impl_libEGL_eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint* v) -> EGLBoolean {
  return eglQuerySurface(dpy, (EGLSurface)UNMAPH(s), a, v);
}
static auto fexfn_impl_libEGL_eglSurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint v) -> EGLBoolean {
  return eglSurfaceAttrib(dpy, (EGLSurface)UNMAPH(s), a, v);
}
static auto fexfn_impl_libEGL_eglQueryContext(EGLDisplay dpy, EGLContext c, EGLint a, EGLint* v) -> EGLBoolean {
  return eglQueryContext(dpy, (EGLContext)UNMAPH(c), a, v);
}
static auto fexfn_impl_libEGL_eglBindTexImage(EGLDisplay dpy, EGLSurface s, EGLint b) -> EGLBoolean {
  return eglBindTexImage(dpy, (EGLSurface)UNMAPH(s), b);
}
static auto fexfn_impl_libEGL_eglReleaseTexImage(EGLDisplay dpy, EGLSurface s, EGLint b) -> EGLBoolean {
  return eglReleaseTexImage(dpy, (EGLSurface)UNMAPH(s), b);
}
static auto fexfn_impl_libEGL_eglGetCurrentContext() -> EGLContext {
  return (EGLContext)MAPH(eglGetCurrentContext());
}
static auto fexfn_impl_libEGL_eglGetCurrentSurface(EGLint rd) -> EGLSurface {
  return (EGLSurface)MAPH(eglGetCurrentSurface(rd));
}


EXPORTS(libEGL)
