/* memtypes - what memory does this Vulkan driver actually offer, and how fast is it?
 *
 * Written to explain one number: on the open pvr driver glReadPixels costs ~2.9 ms
 * fixed + 2.3 ms/MB against the vendor's ~0.65 + 0.77. This splits the two halves of
 * a readback - the GPU copy into a host-visible buffer, and the CPU read of that
 * buffer - and prints every memory type the driver exposes with its flags.
 *
 * Run with VK_ICD_FILENAMES pointing at whichever driver you want to measure.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define DIE(...)                                                                        \
    do {                                                                                \
        fprintf(stderr, "FAIL: " __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                          \
        exit(1);                                                                        \
    } while (0)

#define VKCHECK(x)                                                                      \
    do {                                                                                \
        VkResult _r = (x);                                                              \
        if (_r != VK_SUCCESS)                                                           \
            DIE("%s -> %d", #x, (int)_r);                                               \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char *type_flags(VkMemoryPropertyFlags f, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s%s%s%s", (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
             (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
             (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
             (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
             (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZILY_ALLOCATED " : "");
    return buf;
}

int main(int argc, char **argv)
{
    size_t bytes = 1024 * 1024;
    int iters = 20;
    if (argc > 1)
        bytes = (size_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        iters = atoi(argv[2]);

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst;
    VKCHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    if (!ndev)
        DIE("no Vulkan device (is the ICD's driver loaded?)");
    VkPhysicalDevice *pdevs = calloc(ndev, sizeof(*pdevs));
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, pdevs));
    VkPhysicalDevice pd = pdevs[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);

    printf("%s (api %u.%u.%u) - %u heap(s), %u memory type(s)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), mem.memoryHeapCount, mem.memoryTypeCount);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        printf("  heap %u: %.1f MiB%s\n", i, mem.memoryHeaps[i].size / 1048576.0,
               (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device local)" : "");
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        char buf[128];
        printf("  type %u: heap %u  %s\n", i, mem.memoryTypes[i].heapIndex,
               type_flags(mem.memoryTypes[i].propertyFlags, buf, sizeof(buf)));
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    VKCHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* device-local source buffer */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer src;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &src));
    VkMemoryRequirements sreq;
    vkGetBufferMemoryRequirements(dev, src, &sreq);
    uint32_t stype = ~0u;
    for (uint32_t i = 0; i < mem.memoryTypeCount && stype == ~0u; i++)
        if (sreq.memoryTypeBits & (1u << i))
            stype = i;
    if (stype == ~0u)
        DIE("no memory type for the source buffer");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = sreq.size, .memoryTypeIndex = stype };
    VkDeviceMemory smem;
    VKCHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
    VKCHECK(vkBindBufferMemory(dev, src, smem, 0));

    printf("\n%zu KiB transfer, %d iterations per memory type\n", bytes / 1024, iters);

    for (uint32_t t = 0; t < mem.memoryTypeCount; t++) {
        if (!(mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;

        VkBuffer dst;
        VKCHECK(vkCreateBuffer(dev, &bci, NULL, &dst));
        VkMemoryRequirements dreq;
        vkGetBufferMemoryRequirements(dev, dst, &dreq);
        mai.allocationSize = dreq.size;
        mai.memoryTypeIndex = t;
        VkDeviceMemory dmem;
        if (vkAllocateMemory(dev, &mai, NULL, &dmem) != VK_SUCCESS) {
            printf("  type %u: cannot allocate a host-visible buffer\n", t);
            vkDestroyBuffer(dev, dst, NULL);
            continue;
        }
        VKCHECK(vkBindBufferMemory(dev, dst, dmem, 0));

        double t_copy = 0, t_read = 0, t_write = 0;
        for (int it = 0; it < iters; it++) {
            /* GPU: source -> host-visible destination */
            VkCommandPool cp;
            VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                             .queueFamilyIndex = 0 };
            VKCHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
            VkCommandBuffer cb;
            VkCommandBufferAllocateInfo cbai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
            VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
            VkCommandBufferBeginInfo cbbi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            VKCHECK(vkBeginCommandBuffer(cb, &cbbi));
            VkBufferCopy region = { .size = bytes };
            vkCmdCopyBuffer(cb, src, dst, 1, &region);
            VKCHECK(vkEndCommandBuffer(cb));
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence;
            VKCHECK(vkCreateFence(dev, &fci, NULL, &fence));
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1, .pCommandBuffers = &cb };
            double a = now_ms();
            VKCHECK(vkQueueSubmit(q, 1, &si, fence));
            VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
            t_copy += now_ms() - a;

            void *map = NULL;
            VKCHECK(vkMapMemory(dev, dmem, 0, VK_WHOLE_SIZE, 0, &map));
            volatile uint64_t sum = 0;
            const uint8_t *p = map;
            a = now_ms();
            for (size_t i = 0; i < bytes; i += 64)
                sum += p[i];
            t_read += now_ms() - a;
            a = now_ms();
            memset(map, (int)sum, bytes);
            t_write += now_ms() - a;
            vkUnmapMemory(dev, dmem);

            vkDestroyFence(dev, fence, NULL);
            vkDestroyCommandPool(dev, cp, NULL);
        }
        char buf[128];
        printf("  type %u [%s]\n", t, type_flags(mem.memoryTypes[t].propertyFlags, buf, sizeof(buf)));
        printf("      gpu copy + fence : %7.3f ms\n", t_copy / iters);
        printf("      cpu read (%zuKB)  : %7.3f ms  (%.0f MB/s)\n", bytes / 1024,
               t_read / iters, (bytes / 1048576.0) / (t_read / iters / 1000.0));
        printf("      cpu write        : %7.3f ms\n", t_write / iters);
        vkDestroyBuffer(dev, dst, NULL);
        vkFreeMemory(dev, dmem, NULL);
    }

    /* If a driver allocates a fresh staging buffer per readback, the allocation
     * itself is a fixed per-call cost. Time allocate+bind+free for each host-visible
     * type, with and without a first CPU touch. */
    {
        printf("\nallocation cost (%zu KiB buffers)\n", bytes / 1024);
        for (uint32_t t = 0; t < mem.memoryTypeCount; t++) {
            if (!(mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;
            VkBuffer b;
            VkMemoryRequirements req;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VKCHECK(vkCreateBuffer(dev, &bci, NULL, &b));
            vkGetBufferMemoryRequirements(dev, b, &req);
            double t_alloc = 0, t_touch = 0;
            int n = 20;
            for (int i = 0; i < n; i++) {
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = t;
                VkDeviceMemory m;
                double a = now_ms();
                if (vkAllocateMemory(dev, &mai, NULL, &m) != VK_SUCCESS)
                    break;
                VKCHECK(vkBindBufferMemory(dev, b, m, 0));
                t_alloc += now_ms() - a;
                void *map = NULL;
                a = now_ms();
                VKCHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &map));
                memset(map, i, 4096); /* first touch of the first page only */
                vkUnmapMemory(dev, m);
                t_touch += now_ms() - a;
                vkFreeMemory(dev, m, NULL);
            }
            printf("  type %u: allocate+bind %6.3f ms | map+touch(4K) %6.3f ms\n", t, t_alloc / n,
                   t_touch / n);
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkDestroyBuffer(dev, b, NULL);
        }
    }

    /* The readback shape a GL driver uses: render target -> host buffer -> CPU read.
     * Splitting the three parts says which one dominates, which the buffer-to-buffer
     * numbers above cannot, because a readback also pays for the image copy and for
     * waiting on the fence. */
    {
        uint32_t w = 512, h = 512;
        const char *sz = getenv("READBACK_SIZE");
        if (sz) {
            w = h = (uint32_t)strtoul(sz, NULL, 0);
            if (w < 16 || w > 4096)
                DIE("READBACK_SIZE out of range");
        }
        size_t img_bytes = (size_t)w * h * 4;

        VkImageCreateInfo ici2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                   .imageType = VK_IMAGE_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                                   .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .tiling = VK_IMAGE_TILING_LINEAR,
                                   .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage img;
        VKCHECK(vkCreateImage(dev, &ici2, NULL, &img));
        VkMemoryRequirements ireq;
        vkGetImageMemoryRequirements(dev, img, &ireq);
        uint32_t itype = ~0u;
        for (uint32_t i = 0; i < mem.memoryTypeCount && itype == ~0u; i++)
            if (ireq.memoryTypeBits & (1u << i))
                itype = i;
        mai.allocationSize = ireq.size;
        mai.memoryTypeIndex = itype;
        VkDeviceMemory imem;
        VKCHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
        VKCHECK(vkBindImageMemory(dev, img, imem, 0));

        printf("\nreadback %ux%u (%.0f KiB) - image copy, fence wait and CPU read\n", w, h,
               img_bytes / 1024.0);
        for (uint32_t t = 0; t < mem.memoryTypeCount; t++) {
            if (!(mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;

            VkBufferCreateInfo rbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = img_bytes,
                                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            VkBuffer rbuf;
            VKCHECK(vkCreateBuffer(dev, &rbci, NULL, &rbuf));
            VkMemoryRequirements rreq;
            vkGetBufferMemoryRequirements(dev, rbuf, &rreq);
            mai.allocationSize = rreq.size;
            mai.memoryTypeIndex = t;
            VkDeviceMemory rmem;
            if (vkAllocateMemory(dev, &mai, NULL, &rmem) != VK_SUCCESS) {
                vkDestroyBuffer(dev, rbuf, NULL);
                continue;
            }
            VKCHECK(vkBindBufferMemory(dev, rbuf, rmem, 0));

            VkCommandPool cp;
            VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                             .queueFamilyIndex = 0 };
            VKCHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
            VkCommandBuffer cb;
            VkCommandBufferAllocateInfo cbai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
            VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence;
            VKCHECK(vkCreateFence(dev, &fci, NULL, &fence));

            double t_rec = 0, t_wait = 0, t_cpu = 0;
            for (int it = 0; it < iters; it++) {
                VkCommandBufferBeginInfo cbbi = {
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
                double a = now_ms();
                VKCHECK(vkResetCommandBuffer(cb, 0));
                VKCHECK(vkBeginCommandBuffer(cb, &cbbi));
                VkBufferImageCopy region = {
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageExtent = { w, h, 1 } };
                vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, rbuf, 1, &region);
                VKCHECK(vkEndCommandBuffer(cb));
                t_rec += now_ms() - a;

                VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                    .commandBufferCount = 1, .pCommandBuffers = &cb };
                VKCHECK(vkResetFences(dev, 1, &fence));
                VKCHECK(vkQueueSubmit(q, 1, &si, fence));
                a = now_ms();
                VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
                t_wait += now_ms() - a;

                void *map = NULL;
                VKCHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &map));
                volatile uint64_t sum = 0;
                const uint8_t *p = map;
                a = now_ms();
                for (size_t i = 0; i < img_bytes; i += 64)
                    sum += p[i];
                t_cpu += now_ms() - a;
                vkUnmapMemory(dev, rmem);
            }
            printf("  type %u: record %6.3f | fence wait %6.3f | cpu read %6.3f | total %6.3f ms\n",
                   t, t_rec / iters, t_wait / iters, t_cpu / iters,
                   (t_rec + t_wait + t_cpu) / iters);
            vkDestroyFence(dev, fence, NULL);
            vkDestroyCommandPool(dev, cp, NULL);
            vkDestroyBuffer(dev, rbuf, NULL);
            vkFreeMemory(dev, rmem, NULL);
        }

        vkDestroyImage(dev, img, NULL);
        vkFreeMemory(dev, imem, NULL);
    }

    vkDestroyBuffer(dev, src, NULL);
    vkFreeMemory(dev, smem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
