/* vkalloc-limit.c - how many VkDeviceMemory objects can actually be held at once?
 * Allocates 4 KB at a time until failure, so total memory stays tiny (4096 allocs = 16 MB)
 * and the COUNT is the only thing being measured.
 * ponytail: leaks on purpose - it exits right after counting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("instance FAILED"); return 1; }

    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) { puts("no devices"); return 1; }
    VkPhysicalDevice *pds = calloc(nd, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device      : %s\n", props.deviceName);
    printf("declared    : maxMemoryAllocationCount = %u\n", props.limits.maxMemoryAllocationCount);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq, sizeof(*q));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    uint32_t qi = 0;
    for (uint32_t i = 0; i < nq; i++) if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { puts("device create FAILED"); return 1; }

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    /* pick the first host-visible type - small allocations, so placement does not matter */
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { mt = i; break; }

    unsigned long n = 0;
    VkResult r = VK_SUCCESS;
    for (;;) {
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = 4096; ai.memoryTypeIndex = mt;
        VkDeviceMemory m;
        r = vkAllocateMemory(dev, &ai, NULL, &m);
        if (r != VK_SUCCESS) break;
        n++;
        if (n >= 20000UL) break;   /* 20000 x 4 KB = 80 MB - 5x the open driver's declared 4096, safe on this board */
    }
    printf("allocated   : %lu objects of 4096 B (%.1f MB)\n", n, n * 4096.0 / 1048576.0);
    printf("stopped by  : VkResult %d%s\n", r,
           r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? " (OUT_OF_DEVICE_MEMORY)" :
           r == VK_ERROR_OUT_OF_HOST_MEMORY   ? " (OUT_OF_HOST_MEMORY)" : "");
    printf("VERDICT     : real limit %s the declared %u\n",
           n >= props.limits.maxMemoryAllocationCount ? "REACHED OR EXCEEDED" : "is BELOW",
           props.limits.maxMemoryAllocationCount);
    return 0;
}
