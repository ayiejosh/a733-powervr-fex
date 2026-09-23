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

    vkDestroyBuffer(dev, src, NULL);
    vkFreeMemory(dev, smem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
