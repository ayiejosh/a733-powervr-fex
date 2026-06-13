/*
 * GO/NO-GO: can the PowerVR GPU write into an externally-imported dma-buf?
 *
 * gbm_bo_create(renderD128, LINEAR)  -> dma-buf fd (export path already works)
 *   -> Vulkan import (VK_EXT_external_memory_dma_buf) into a VkBuffer
 *   -> GPU vkCmdFillBuffer(0xDEADBEEF)
 *   -> CPU mmap the SAME dma-buf -> verify the pattern landed.
 *
 * PASS  => the PMR-from-dma-buf is GPU-renderable; the gem_prime_import port is viable.
 * FAIL  => MMU/firmware won't touch imported pages; the port is dead regardless of plumbing.
 *
 * Build: gcc dmabuf_render_test.c -o /tmp/dmabuf_render_test $(pkg-config --cflags --libs gbm) -lvulkan -ldl
 * Run:   LD_LIBRARY_PATH=/usr/local/lib VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json \
 *        XDG_RUNTIME_DIR=/run/user/1000 /tmp/dmabuf_render_test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

#define W 256
#define H 256
#define FILLVAL 0xDEADBEEFu

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, _r, __LINE__); exit(2); } } while (0)

int main(void)
{
    /* ---- 1. gbm allocate on the render node (linear so CPU can read it) ---- */
    int drmfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drmfd < 0) { perror("open renderD128"); return 2; }

    struct gbm_device *gbm = gbm_create_device(drmfd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 2; }

    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create failed: %s\n", strerror(errno)); return 2; }

    uint32_t stride = gbm_bo_get_stride(bo);
    size_t bo_size = (size_t)stride * H;
    int dfd = gbm_bo_get_fd(bo);     /* PRIME_HANDLE_TO_FD export (already supported) */
    if (dfd < 0) { fprintf(stderr, "gbm_bo_get_fd failed\n"); return 2; }
    printf("gbm bo: %ux%u stride=%u size=%zu dma-buf fd=%d\n", W, H, stride, bo_size, dfd);

    /* ---- 2. Vulkan instance + pick the PowerVR device ---- */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1 };
    const char *iexts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
    VkInstance inst;
    VKCHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    VkPhysicalDevice devs[8]; if (ndev > 8) ndev = 8;
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, devs));
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("  device[%u]: %s (type %d)\n", i, p.deviceName, p.deviceType);
        if (strstr(p.deviceName, "PowerVR") || strstr(p.deviceName, "BXM"))
            phys = devs[i];
    }
    if (phys == VK_NULL_HANDLE) { fprintf(stderr, "no PowerVR device\n"); return 2; }

    /* queue family with transfer/graphics */
    uint32_t nqf = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties qf[8]; if (nqf > 8) nqf = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);
    uint32_t qfi = 0; int found = 0;
    for (uint32_t i = 0; i < nqf; i++)
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) { qfi = i; found = 1; break; }
    if (!found) { fprintf(stderr, "no transfer queue\n"); return 2; }

    /* ---- 3. logical device with dma-buf import extensions ---- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dexts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 3, .ppEnabledExtensionNames = dexts };
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfi, 0, &queue);

    PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdProperties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    if (!pGetMemoryFdProperties) { fprintf(stderr, "no vkGetMemoryFdPropertiesKHR\n"); return 2; }

    /* ---- 4. VkBuffer that will be backed by the imported dma-buf ---- */
    VkExternalMemoryBufferCreateInfo extbuf = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &extbuf, .size = bo_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf));

    VkMemoryRequirements mreq; vkGetBufferMemoryRequirements(dev, buf, &mreq);

    VkMemoryFdPropertiesKHR fdProps = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    int impfd = dup(dfd);   /* Vulkan import consumes the fd; keep dfd for CPU mmap */
    VKCHECK(pGetMemoryFdProperties(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, impfd, &fdProps));

    uint32_t typeBits = mreq.memoryTypeBits & fdProps.memoryTypeBits;
    if (!typeBits) {
        fprintf(stderr, "FAIL: no memory type compatible with both buffer and dma-buf "
                "(buf=0x%x dmabuf=0x%x)\n", mreq.memoryTypeBits, fdProps.memoryTypeBits);
        return 3;
    }
    uint32_t memType = __builtin_ctz(typeBits);
    printf("import: bufBits=0x%x dmabufBits=0x%x -> memType=%u allocSize=%llu\n",
           mreq.memoryTypeBits, fdProps.memoryTypeBits, memType,
           (unsigned long long)mreq.size);

    VkImportMemoryFdInfoKHR impInfo = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = impfd };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &impInfo, .allocationSize = mreq.size, .memoryTypeIndex = memType };
    VkDeviceMemory mem;
    VKCHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));
    printf("imported dma-buf bound to VkBuffer OK\n");

    /* ---- 5. GPU writes the pattern: vkCmdFillBuffer ---- */
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfi };
    VkCommandPool pool; VKCHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(cmd, &bi));
    vkCmdFillBuffer(cmd, buf, 0, (bo_size & ~3ull), FILLVAL);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(queue));
    printf("GPU vkCmdFillBuffer(0x%08X) submitted + completed\n", FILLVAL);

    /* ---- 6. CPU reads the SAME dma-buf and checks the pattern ---- */
    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(dfd, DMA_BUF_IOCTL_SYNC, &s);   /* best-effort cache sync */
    void *map = mmap(NULL, bo_size, PROT_READ, MAP_SHARED, dfd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(dma-buf) failed: %s -- falling back to gbm_bo_map\n", strerror(errno));
        void *mapdata = NULL; uint32_t mstride = 0;
        void *p = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &mstride, &mapdata);
        if (!p) { fprintf(stderr, "gbm_bo_map also failed\n"); return 2; }
        map = p; bo_size = (size_t)mstride * H;
    }
    volatile uint32_t *words = (volatile uint32_t *)map;
    size_t nwords = bo_size / 4;
    size_t ok = 0, bad = 0;
    uint32_t firstbad = 0; size_t firstbadidx = 0;
    for (size_t i = 0; i < nwords; i++) {
        if (words[i] == FILLVAL) ok++;
        else { if (!bad) { firstbad = words[i]; firstbadidx = i; } bad++; }
    }
    struct dma_buf_sync e = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
    ioctl(dfd, DMA_BUF_IOCTL_SYNC, &e);

    printf("readback: %zu/%zu words == 0x%08X  (mismatch %zu)\n", ok, nwords, FILLVAL, bad);
    printf("sample[0..3] = 0x%08X 0x%08X 0x%08X 0x%08X\n",
           words[0], words[1], words[2], words[3]);
    if (bad) printf("first mismatch @word %zu = 0x%08X\n", firstbadidx, firstbad);

    if (ok == nwords) {
        printf("\n=== PASS: GPU wrote the full pattern into the imported dma-buf ===\n");
        return 0;
    } else if (ok > 0) {
        printf("\n=== PARTIAL: GPU wrote some pattern (%zu/%zu) -- inspect tiling/size ===\n", ok, nwords);
        return 1;
    } else {
        printf("\n=== NO-GO: GPU did NOT write the imported dma-buf (all mismatch) ===\n");
        return 1;
    }
}
