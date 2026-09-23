# Mesa pvr: real vkFlush/vkInvalidateMappedMemoryRanges, and an honest cached type

The last piece of the cached-memory-type work
([§21.10-§21.12 of the GPU report](../docs/GPU-RESEARCH-2026-09-22.md) diagnosed the cause and fixed the
kernel half). This is the userspace half, and it closes the item.

## What was wrong

The driver offered a CPU-cacheable host-visible memory type, mapped it cacheable for the readback
throughput (2613 MB/s against 340 MB/s for the write-combined type), and then advertised
`VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` on it. That claim was the bug: the CPU could leave an upload
sitting in its cache while the GPU read stale DRAM. Measured through zink, GL rendered visibly wrong -
59408 wrong pixels of 262144, and 8032/528/2144 after the kernel-side map/unmap sync was fixed. The
type was therefore made opt-in (`PVR_ENABLE_CACHED_MEMORY_TYPE=1`) rather than fixed.

## The change

1. **`pvr_FlushMappedMemoryRanges()` / `pvr_InvalidateMappedMemoryRanges()` are implemented**
   (`pvr_device.c`). Both were `return VK_SUCCESS;`. They now export the BO as a dma-buf and issue
   `DMA_BUF_IOCTL_SYNC`:
   - flush = `DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE` (CPU wrote, device will read)
   - invalidate = `DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ` (device wrote, CPU will read)

   That lands in the exporter's `begin_cpu_access()`/`end_cpu_access()`, and the kernel's
   `drm_gem_shmem` helpers implement those with `dma_sync_sgtable_for_cpu()`/`for_device()` - over the
   whole object, which is coarser than the requested range but correct. **No kernel change was
   needed**: the BO is already exported as a dma-buf and Mesa already had a `buffer_get_fd` winsys
   operation, so the standard dma-buf sync ioctl is the whole mechanism.
2. **`HOST_COHERENT_BIT` dropped from the cached type** (`pvr_physical_device.c`). The type is now
   declared for what it is - `DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED` - so applications know they
   must flush and invalidate, which is exactly what they now can do.

## Verification

```
$ PVR_ENABLE_CACHED_MEMORY_TYPE=1 ./memtypes
  type 0: heap 0  DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT
  type 1: heap 0  DEVICE_LOCAL HOST_VISIBLE HOST_CACHED        <- no longer lies
      cpu read (1024KB)  :   2.938 ms  (340 MB/s)              <- write-combined
      cpu read (1024KB)  :   0.383 ms  (2613 MB/s)             <- cached, 7.7x

$ PVR_ENABLE_CACHED_MEMORY_TYPE=1 glheadless 512 40   (zink over the open driver)
  RESULT: PASS - 262144/262144 pixels correct          <- was 8032, 528, 2144 wrong
$ regress.sh
  regress: 22 passed, 0 failed, 0 known-open           (now includes the cached-type GL case)
```

`regress.sh` gained a permanent case for this: `glheadless` with the cached type enabled, so a future
change that breaks the flush/invalidate path fails the suite instead of quietly corrupting GL.

## Still a policy decision, not a correctness one

The type is still opt-in. With the flush/invalidate path real, enabling it by default is now a
question of whether the applications you care about flush correctly rather than whether the driver can
honour them - which is a different, much safer question than it was.
