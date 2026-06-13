/*
$info$
tags: thunklibs|va
$end_info$
*/
#include <va/va.h>
#include <va/va_drm.h>
#include "common/Host.h"
#include <dlfcn.h>

#include "thunkgen_host_libva.inl"

// vaGetDisplayDRM lives in libva-drm.so.2, not libva.so.2. The generated loader resolves
// symbols via dlsym(RTLD_DEFAULT), so make libva-drm globally visible before it runs.
__attribute__((constructor)) static void fex_libva_preload_drm() {
  dlopen("libva-drm.so.2", RTLD_GLOBAL | RTLD_LAZY);
}

EXPORTS(libva)
