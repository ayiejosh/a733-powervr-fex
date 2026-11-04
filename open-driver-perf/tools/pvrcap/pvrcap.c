/* pvrcap: LD_PRELOAD capture of powervr DRM ioctls for replay/bisection.
 * PVRCAP_DIR=<dir>      write <dir>/cap.bin (records: u32 type, u32 len, payload)
 * PVRCAP_FORCE_CPU=1    add ALLOW_CPU_USERSPACE_ACCESS to BOs that are not PM_FW_PROTECT, so they can be dumped
 * At every SUBMIT_JOBS that contains a geometry job, every mapped CPU-accessible range is dumped (by GPU VA).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "drm.h"
#include "pvr_drm.h"

enum { R_BO = 1, R_MAP, R_UNMAP, R_VMCTX, R_CTX, R_FL, R_HWRT, R_SUBMIT, R_MEM, R_CLOSE, R_SYNCOBJ, R_DESTROY, R_SUBMIT_RET };

static int (*real_ioctl)(int, unsigned long, ...);
static FILE *out;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int force_cpu;

#define MAXBO 65536
struct bo { uint64_t size, flags; void *map; int fd; };
static struct bo bos[MAXBO];
#define MAXMAP 65536
struct mp { uint64_t addr, size, offset; uint32_t handle, live; };
static struct mp maps[MAXMAP];
static int nmaps;

static void rec(uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
   uint32_t hdr[2] = { type, alen + blen };
   fwrite(hdr, 8, 1, out);
   if (alen) fwrite(a, alen, 1, out);
   if (blen) fwrite(b, blen, 1, out);
}

__attribute__((constructor)) static void init(void)
{
   real_ioctl = dlsym(RTLD_NEXT, "ioctl");
   const char *d = getenv("PVRCAP_DIR");
   if (!d) return;
   char p[512];
   snprintf(p, sizeof p, "%s/cap.bin", d);
   out = fopen(p, "wb");
   force_cpu = getenv("PVRCAP_FORCE_CPU") != NULL;
}

__attribute__((destructor)) static void fini(void) { if (out) fclose(out); }

static void dump_mem(int fd)
{
   for (int i = 0; i < nmaps; i++) {
      struct mp *m = &maps[i];
      if (!m->live || m->handle >= MAXBO) continue;
      struct bo *b = &bos[m->handle];
      if (!(b->flags & DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS)) continue;
      if (!b->map) {
         struct drm_pvr_ioctl_get_bo_mmap_offset_args a = { .handle = m->handle };
         if (real_ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &a)) continue;
         void *p = mmap(NULL, b->size, PROT_READ, MAP_SHARED, fd, a.offset);
         if (p == MAP_FAILED) continue;
         b->map = p;
      }
      uint64_t hdr[2] = { m->addr, m->size };
      rec(R_MEM, hdr, 16, (char *)b->map + m->offset, (uint32_t)m->size);
   }
}

int ioctl(int fd, unsigned long req, ...)
{
   va_list ap;
   va_start(ap, req);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   if (!out) return real_ioctl(fd, req, arg);

   pthread_mutex_lock(&lk);
   int ret;
   switch (req) {
   case DRM_IOCTL_PVR_CREATE_BO: {
      struct drm_pvr_ioctl_create_bo_args *a = arg;
      uint64_t orig = a->flags;
      if (force_cpu && !(a->flags & DRM_PVR_BO_PM_FW_PROTECT))
         a->flags |= DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS;
      ret = real_ioctl(fd, req, arg);
      if (!ret && a->handle < MAXBO) bos[a->handle] = (struct bo){ a->size, a->flags, NULL, fd };
      uint64_t r[4] = { a->size, orig, a->flags, ret ? 0xffffffffu : a->handle };
      rec(R_BO, r, sizeof r, NULL, 0);
      break;
   }
   case DRM_IOCTL_PVR_VM_MAP: {
      struct drm_pvr_ioctl_vm_map_args *a = arg;
      ret = real_ioctl(fd, req, arg);
      if (!ret && nmaps < MAXMAP)
         maps[nmaps++] = (struct mp){ a->device_addr, a->size, a->offset, a->handle, 1 };
      rec(R_MAP, a, sizeof *a, NULL, 0);
      break;
   }
   case DRM_IOCTL_PVR_VM_UNMAP: {
      struct drm_pvr_ioctl_vm_unmap_args *a = arg;
      ret = real_ioctl(fd, req, arg);
      for (int i = 0; i < nmaps; i++)
         if (maps[i].live && maps[i].addr == a->device_addr) maps[i].live = 0;
      rec(R_UNMAP, a, sizeof *a, NULL, 0);
      break;
   }
   case DRM_IOCTL_PVR_CREATE_VM_CONTEXT:
      ret = real_ioctl(fd, req, arg);
      rec(R_VMCTX, arg, sizeof(struct drm_pvr_ioctl_create_vm_context_args), NULL, 0);
      break;
   case DRM_IOCTL_PVR_CREATE_CONTEXT: {
      struct drm_pvr_ioctl_create_context_args *a = arg;
      ret = real_ioctl(fd, req, arg);
      rec(R_CTX, a, sizeof *a, (void *)(uintptr_t)a->static_context_state, a->static_context_state_len);
      break;
   }
   case DRM_IOCTL_PVR_CREATE_FREE_LIST:
      ret = real_ioctl(fd, req, arg);
      rec(R_FL, arg, sizeof(struct drm_pvr_ioctl_create_free_list_args), NULL, 0);
      break;
   case DRM_IOCTL_PVR_CREATE_HWRT_DATASET:
      ret = real_ioctl(fd, req, arg);
      rec(R_HWRT, arg, sizeof(struct drm_pvr_ioctl_create_hwrt_dataset_args), NULL, 0);
      break;
   case DRM_IOCTL_PVR_DESTROY_CONTEXT:
   case DRM_IOCTL_PVR_DESTROY_FREE_LIST:
   case DRM_IOCTL_PVR_DESTROY_HWRT_DATASET:
   case DRM_IOCTL_PVR_DESTROY_VM_CONTEXT: {
      uint32_t r[2] = { (uint32_t)_IOC_NR(req), *(uint32_t *)arg };
      rec(R_DESTROY, r, 8, NULL, 0);
      ret = real_ioctl(fd, req, arg);
      break;
   }
   case DRM_IOCTL_SYNCOBJ_CREATE:
      ret = real_ioctl(fd, req, arg);
      rec(R_SYNCOBJ, arg, sizeof(struct drm_syncobj_create), NULL, 0);
      break;
   case DRM_IOCTL_GEM_CLOSE: {
      struct drm_gem_close *a = arg;
      if (a->handle < MAXBO && bos[a->handle].map) {
         munmap(bos[a->handle].map, bos[a->handle].size);
         bos[a->handle].map = NULL;
      }
      rec(R_CLOSE, &a->handle, 4, NULL, 0);
      ret = real_ioctl(fd, req, arg);
      break;
   }
   case DRM_IOCTL_PVR_SUBMIT_JOBS: {
      struct drm_pvr_ioctl_submit_jobs_args *a = arg;
      const char *jobs = (const char *)(uintptr_t)a->jobs.array;
      int has_geom = 0;
      for (uint32_t i = 0; i < a->jobs.count; i++)
         if (((const struct drm_pvr_job *)(jobs + i * a->jobs.stride))->type == DRM_PVR_JOB_TYPE_GEOMETRY)
            has_geom = 1;
      if (has_geom) dump_mem(fd);
      uint32_t h[2] = { a->jobs.count, a->jobs.stride };
      rec(R_SUBMIT, h, 8, NULL, 0);
      for (uint32_t i = 0; i < a->jobs.count; i++) {
         const struct drm_pvr_job *j = (const void *)(jobs + i * a->jobs.stride);
         rec(R_SUBMIT, j, sizeof *j, (void *)(uintptr_t)j->cmd_stream, j->cmd_stream_len);
         rec(R_SUBMIT, &j->sync_ops, sizeof j->sync_ops, (void *)(uintptr_t)j->sync_ops.array,
             j->sync_ops.count * j->sync_ops.stride);
      }
      ret = real_ioctl(fd, req, arg);
      int e = ret ? errno : 0;
      rec(R_SUBMIT_RET, &e, 4, NULL, 0);
      fflush(out);
      break;
   }
   default:
      ret = real_ioctl(fd, req, arg);
   }
   pthread_mutex_unlock(&lk);
   return ret;
}
