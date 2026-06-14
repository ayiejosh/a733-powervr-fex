// Standalone EGL thunk interface with 64<->32 handle mapping via custom_host_impl.
// Local-only (NOT a FEX contribution). Handle-touching functions are custom_host_impl;
// the host .cpp maps EGLConfig/EGLContext/EGLSurface for 32-bit guests.
#include <common/GeneratorInterface.h>
#include <EGL/egl.h>

template<auto>
struct fex_gen_config {
  unsigned version = 1;
};
template<auto, int, typename = void>
struct fex_gen_param {};

// --- passthrough functions (no EGLConfig/Context/Surface handle to map) ---
template<> struct fex_gen_config<eglBindAPI> {};
template<> struct fex_gen_config<eglInitialize> {};
template<> struct fex_gen_config<eglTerminate> {};
template<> struct fex_gen_config<eglGetError> {};
template<> struct fex_gen_config<eglGetCurrentDisplay> {};
template<> struct fex_gen_config<eglQueryString> {};
template<> struct fex_gen_config<eglSwapInterval> {};
template<> struct fex_gen_config<eglWaitGL> {};
template<> struct fex_gen_config<eglWaitNative> {};
template<> struct fex_gen_config<eglWaitClient> {};
template<> struct fex_gen_config<eglReleaseThread> {};

// EGLNativeDisplayType is a pointer to opaque data (wl_display/(X)Display/...)
template<> struct fex_gen_config<eglGetDisplay> {};
template<> struct fex_gen_param<eglGetDisplay, 0, EGLNativeDisplayType> : fexgen::assume_compatible_data_layout {};

// --- handle-mapping functions (custom host impl does the 64<->32 mapping) ---
template<> struct fex_gen_config<eglChooseConfig> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglGetConfigs> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglGetConfigAttrib> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglCreateContext> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglDestroyContext> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglCreatePbufferSurface> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglCreateWindowSurface> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglDestroySurface> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglMakeCurrent> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglSwapBuffers> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglQuerySurface> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglSurfaceAttrib> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglQueryContext> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglBindTexImage> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglReleaseTexImage> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglGetCurrentContext> : fexgen::custom_host_impl {};
template<> struct fex_gen_config<eglGetCurrentSurface> : fexgen::custom_host_impl {};
