/* Tiny LD_PRELOAD logger: dumps every DRM ioctl request number and the first bytes of its argument
 * struct, and decodes DRM_IOCTL_PVR_SUBMIT_JOBS fully (jobs plus their sync ops). Used to diff the
 * per-client UAPI call sequences of the native probe and the open Vulkan driver. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DRM_IOCTL_TYPE 'd'
#define DRM_COMMAND_BASE 0x40
#define NR_SUBMIT 0x4d
#define NR_CREATE_CONTEXT 0x47

struct create_context_args {
   uint32_t type, flags;
   int32_t priority;
   uint32_t handle;
   uint64_t static_context_state;
   uint32_t static_context_state_len;
   uint32_t vm_context_handle;
   uint64_t callstack_addr;
};

struct obj_array { uint32_t stride; uint32_t count; uint64_t array; };
struct sync_op { uint32_t handle; uint32_t flags; uint64_t value; };
struct hwrt_ref { uint32_t set_handle; uint32_t data_index; };
struct pvr_job {
   uint32_t type, context_handle, flags, cmd_stream_len;
   uint64_t cmd_stream;
   struct obj_array sync_ops;
   struct hwrt_ref hwrt;
};
struct submit_jobs_args { struct obj_array jobs; };

static int (*real_ioctl)(int, unsigned long, void *);
static unsigned long calls;

static void dump_submit(const struct submit_jobs_args *a)
{
   const char *base = (const char *)(uintptr_t)a->jobs.array;
   uint32_t i, j;

   for (i = 0; i < a->jobs.count; i++) {
      const struct pvr_job *job = (const void *)(base + (size_t)i * a->jobs.stride);
      const struct sync_op *ops = (const void *)(uintptr_t)job->sync_ops.array;

      fprintf(stderr, "PVRJOB %lu job%u type=%u ctx=%u flags=%08x cmd=%lx:%u hwrt=%u/%u nops=%u\n",
              calls, i, job->type, job->context_handle, job->flags,
              (unsigned long)job->cmd_stream, job->cmd_stream_len,
              job->hwrt.set_handle, job->hwrt.data_index, job->sync_ops.count);
      for (j = 0; j < job->sync_ops.count; j++)
         fprintf(stderr, "PVRJOBOP %lu job%u op%u handle=%u flags=%08x value=%lu\n",
                 calls, i, j, ops[j].handle, ops[j].flags,
                 (unsigned long)ops[j].value);
      {
         const unsigned char *st = (const unsigned char *)(uintptr_t)job->cmd_stream;
         unsigned long k, n = job->cmd_stream_len;

         if (st != NULL && n <= 1024) {
            char sh[2 * 1024 + 2];
            for (k = 0; k < n; k++)
               snprintf(sh + 2 * k, 4, "%02x", st[k]);
            sh[2 * n] = 0;
            fprintf(stderr, "PVRJOBSTREAM %lu job%u len=%lu bytes=%s\n", calls, i, n, sh);
         }
      }
   }
}

int ioctl(int fd, unsigned long request, ...)
{
   va_list ap;
   void *arg;
   unsigned dir, size, type, nr;

   va_start(ap, request);
   arg = va_arg(ap, void *);
   va_end(ap);

   if (!real_ioctl)
      real_ioctl = dlsym(RTLD_NEXT, "ioctl");

   dir = (request >> 30) & 3;
   size = (request >> 16) & 0x3fff;
   type = (request >> 8) & 0xff;
   nr = request & 0xff;

   if (type == DRM_IOCTL_TYPE && nr >= DRM_COMMAND_BASE) {
      char hex[520];
      unsigned i, n = size < 256 ? size : 256;

      hex[0] = 0;
      for (i = 0; i < n && arg; i++)
         snprintf(hex + 2 * i, 4, "%02x", ((const unsigned char *)arg)[i]);
      fprintf(stderr, "PVRIOCTL %lu fd=%d req=%08lx dir=%u size=%u nr=%02x arg=%s\n",
              calls, fd, request, dir, size, nr, hex);
      if (nr == NR_SUBMIT && arg)
         dump_submit((const struct submit_jobs_args *)arg);
      if (nr == NR_CREATE_CONTEXT && arg) {
         const struct create_context_args *ca = arg;
         fprintf(stderr, "PVRCTX type=%u flags=%x prio=%d scs=0x%lx len=%u vm=%u callstack=0x%lx\n",
                 ca->type, ca->flags, ca->priority,
                 (unsigned long)ca->static_context_state, ca->static_context_state_len,
                 ca->vm_context_handle, (unsigned long)ca->callstack_addr);
         if (ca->static_context_state && ca->static_context_state_len <= 512) {
            const unsigned char *st = (const unsigned char *)(uintptr_t)ca->static_context_state;
            char sh[2 * 512 + 2];
            unsigned long k;
            for (k = 0; k < ca->static_context_state_len; k++)
               snprintf(sh + 2 * k, 4, "%02x", st[k]);
            sh[2 * ca->static_context_state_len] = 0;
            fprintf(stderr, "PVRCTXSTATE len=%u bytes=%s\n", ca->static_context_state_len, sh);
         }
      }
      calls++;
   }

   return real_ioctl(fd, request, arg);
}
