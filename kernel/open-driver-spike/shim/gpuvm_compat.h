/* Backport shim: helpers that exist in v6.8's include/drm/drm_gem.h but not in this
 * 6.6 BSP kernel. Needed only to build our out-of-tree drm_gpuvm/powervr modules.
 * Source: torvalds/linux v6.8 include/drm/drm_gem.h */
#ifndef _KS_GPUVM_COMPAT_H
#define _KS_GPUVM_COMPAT_H
#include <drm/drm_gem.h>
#define drm_gem_for_each_gpuvm_bo(entry__, obj__) \
	list_for_each_entry(entry__, &(obj__)->gpuva.list, list.entry.gem)

#define drm_gem_for_each_gpuvm_bo_safe(entry__, next__, obj__) \
	list_for_each_entry_safe(entry__, next__, &(obj__)->gpuva.list, list.entry.gem)
#endif

/* Size macros added after 6.6 (used by the open PowerVR driver). */
#ifndef SZ_128G
#define SZ_128G (_AC(1, UL) << 30)
#endif
