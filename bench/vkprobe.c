/* Direct PowerVR Vulkan probe: dlopen the IMG ICD, enumerate the GPU.
   Bypasses the old bullseye Vulkan loader (1.2) which can't init the 1.3 ICD. */
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGIPA)(VkInstance, const char*);

int main(void) {
    void *h = dlopen("/usr/lib/libVK_IMG.so.24.2.6603887", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen fail: %s\n", dlerror()); return 1; }
    PFN_icdGIPA gipa = (PFN_icdGIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

    PFN_vkCreateInstance pCreate = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = pCreate(&ici, NULL, &inst);
    printf("vkCreateInstance rc=%d\n", r);
    if (r != VK_SUCCESS) return 2;

    PFN_vkEnumeratePhysicalDevices pEnum = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties pProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties pMem = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    uint32_t n = 0; pEnum(inst, &n, NULL);
    printf("physical devices: %u\n", n);
    if (!n) { printf("NO GPU enumerated\n"); return 3; }
    VkPhysicalDevice dev[8]; if (n > 8) n = 8; pEnum(inst, &n, dev);
    const char *types[] = {"OTHER","INTEGRATED_GPU","DISCRETE_GPU","VIRTUAL_GPU","CPU"};
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr; pProps(dev[i], &pr);
        printf("  GPU[%u]: \"%s\"  type=%s  vulkan=%u.%u.%u  driverVer=0x%x\n",
               i, pr.deviceName, pr.deviceType <= 4 ? types[pr.deviceType] : "?",
               VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion), VK_VERSION_PATCH(pr.apiVersion),
               pr.driverVersion);
        VkPhysicalDeviceMemoryProperties mp; pMem(dev[i], &mp);
        unsigned long long vram = 0;
        for (uint32_t j = 0; j < mp.memoryHeapCount; j++)
            if (mp.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[j].size;
        printf("         device-local memory: %llu MB\n", vram / (1024*1024));
    }
    printf("RESULT: PowerVR Vulkan userspace WORKS\n");
    return 0;
}
