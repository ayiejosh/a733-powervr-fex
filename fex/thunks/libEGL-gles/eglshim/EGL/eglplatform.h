/* platform-agnostic eglplatform.h shim: native handles as void* (no X11 Display*)
   matches FEX's assume_compatible_data_layout intent and avoids thunkgen crash */
#ifndef __eglplatform_h_
#define __eglplatform_h_
#include <KHR/khrplatform.h>
typedef khronos_int32_t EGLint;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef void *EGLNativePixmapType;
typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativeWindowType  NativeWindowType;
typedef EGLNativePixmapType  NativePixmapType;
#define EGLAPI
#define EGLAPIENTRY
#define EGLAPIENTRYP EGLAPIENTRY*
#endif
#ifndef EGL_CAST
#ifdef __cplusplus
#define EGL_CAST(type, value) (static_cast<type>(value))
#else
#define EGL_CAST(type, value) ((type) (value))
#endif
#endif
