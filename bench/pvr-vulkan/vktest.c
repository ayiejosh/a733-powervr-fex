/*
 * vktest.c - does the PowerVR Vulkan driver actually execute work?
 *
 * Why this exists: enumerating a device only proves a driver loaded its
 * entrypoints. On this board we had a GPU with no usable Vulkan at all, so the
 * question that matters is narrower and harder: can userspace get a device, hand
 * it a shader, and read back the right answer? This test answers exactly that,
 * and nothing else:
 *
 *   instance -> physical device -> device+compute queue -> storage buffer
 *   -> compute pipeline from SPIR-V -> dispatch -> fence -> host readback
 *   -> compare every element against the same xorshift the shader ran
 *
 * Then it repeats the dispatch `iters` times to report throughput, because a
 * one-shot dispatch tells you it works but not whether it is usable.
 *
 * Build: see build.sh (glslangValidator + xxd embed the SPIR-V).
 * Run:   VK_ICD_FILENAMES=<icd.json> ./vktest [iters] [elements]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "compute_spv.h"

#define DIE(...)                                    \
    do {                                            \
        fprintf(stderr, "FAIL: " __VA_ARGS__);      \
        fputc('\n', stderr);                        \
        exit(1);                                    \
    } while (0)

#define VKCHECK(expr)                                                        \
    do {                                                                     \
        VkResult r_ = (expr);                                                \
        if (r_ != VK_SUCCESS)                                                \
            DIE("%s -> VkResult %d", #expr, (int)r_);                        \
    } while (0)

static uint32_t xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static const char *device_type_name(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

int main(int argc, char **argv)
{
    uint32_t count = 1u << 20; /* 4 MiB of results */
    int iters = 1;

    if (argc > 1)
        iters = atoi(argv[1]);
    if (argc > 2)
        count = (uint32_t)strtoul(argv[2], NULL, 0);
    if (iters < 1 || count == 0)
        DIE("bad arguments: iters=%d elements=%u", iters, count);

    const VkDeviceSize bytes = (VkDeviceSize)count * sizeof(uint32_t);

    /* ---- instance ------------------------------------------------------- */
    /* The API version is not cosmetic: the loader rejects vkCreateInstance with
     * VK_ERROR_INCOMPATIBLE_DRIVER when the app asks for more than the ICD's JSON
     * declares, and Mesa 25.0.7's pvr ICD declares 1.0 (its meson.build passes
     * --api-version 1.0). So this is settable, and 1.0 by default. */
    uint32_t api = VK_API_VERSION_1_0;
    const char *api_env = getenv("VKTEST_API");
    if (api_env) {
        unsigned maj = 1, min = 0;
        if (sscanf(api_env, "%u.%u", &maj, &min) == 2)
            api = VK_MAKE_VERSION(maj, min, 0);
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pvr-vulkan-test",
        .apiVersion = api,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    VKCHECK(vkCreateInstance(&ici, NULL, &instance));

    /* ---- physical device ------------------------------------------------ */
    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (ndev == 0)
        DIE("no Vulkan physical device (is VK_ICD_FILENAMES set?)");

    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("device[%u] name=\"%s\" type=%s api=%u.%u.%u driver=0x%08x vendor=0x%04x device=0x%04x\n",
               i, p.deviceName, device_type_name(p.deviceType),
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID);
        if (phys == VK_NULL_HANDLE)
            phys = devs[i];
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("using: %s\n", props.deviceName);

    /* ---- queue family --------------------------------------------------- */
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);

    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        printf("queue family %u: flags=0x%x count=%u\n", i, qf[i].queueFlags,
               qf[i].queueCount);
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qfi == UINT32_MAX)
            qfi = i;
    }
    if (qfi == UINT32_MAX)
        DIE("no compute queue family");

    /* ---- device --------------------------------------------------------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    /* Ask for nothing optional: the point is to find out what the driver can
     * do by itself, not to make the request easy to satisfy. */
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys, &dci, NULL, &dev));

    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    /* ---- buffer --------------------------------------------------------- */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf));

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    uint32_t mti = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            mti = i;
            break;
        }
    }
    if (mti == UINT32_MAX)
        DIE("no host-visible coherent memory type");
    printf("memory: type %u, heap %u (%lu MB), flags=0x%x\n", mti,
           mp.memoryTypes[mti].heapIndex,
           (unsigned long)(mp.memoryHeaps[mp.memoryTypes[mti].heapIndex].size >> 20),
           mp.memoryTypes[mti].propertyFlags);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mti,
    };
    VkDeviceMemory mem;
    VKCHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));

    void *mapped = NULL;
    VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped));

    /* ---- pipeline ------------------------------------------------------- */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(compute_spv),
        .pCode = compute_spv,
    };
    VkShaderModule sm;
    VKCHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));

    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &b,
    };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl;
    VKCHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName = "main",
        },
        .layout = pl,
    };
    VkPipeline pipe;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    if (pr != VK_SUCCESS)
        DIE("vkCreateComputePipelines -> %d (shader compile on the GPU driver failed)", (int)pr);

    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &dps,
    };
    VkDescriptorPool dp;
    VKCHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet ds;
    VKCHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));

    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = bytes };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ds,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    /* ---- command buffer ------------------------------------------------- */
    VkCommandPoolCreateInfo cpi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfi,
    };
    VkCommandPool cmdpool;
    VKCHECK(vkCreateCommandPool(dev, &cpi, NULL, &cmdpool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);

    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(cmd));

    /* ---- submit --------------------------------------------------------- */
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VKCHECK(vkCreateFence(dev, &fci, NULL, &fence));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    printf("dispatching %d x %u elements (%lu MiB per dispatch)...\n", iters, count,
           (unsigned long)(bytes >> 20));

    double t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        VKCHECK(vkResetFences(dev, 1, &fence));
        VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
        VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
        if (w != VK_SUCCESS)
            DIE("vkWaitForFences after %d dispatch(es) -> %d (GPU never signalled)", i + 1, (int)w);
    }
    double t1 = now_ms();

    /* ---- verify --------------------------------------------------------- */
    const uint32_t *got = mapped;
    uint64_t bad = 0;
    uint32_t first_bad = 0, first_want = 0, first_got = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t want = xorshift(i) + 1u;
        if (got[i] != want) {
            if (bad == 0) {
                first_bad = i;
                first_want = want;
                first_got = got[i];
            }
            bad++;
        }
    }

    double ms_total = t1 - t0;
    double ms_per = ms_total / iters;
    double gbps = ((double)bytes * 2.0 * iters) / (ms_total / 1000.0) / 1e9;

    printf("submit+wait: %.3f ms total, %.3f ms/dispatch\n", ms_total, ms_per);
    printf("throughput:  %.2f GB/s (read+write)\n", gbps);
    if (bad) {
        printf("RESULT: FAIL - %llu/%u elements wrong (first at %u: want %u got %u)\n",
               (unsigned long long)bad, count, first_bad, first_want, first_got);
    } else {
        printf("RESULT: PASS - %u/%u elements correct\n", count, count);
    }
    printf("VERDICT: %s\n", bad ? "FAIL" : "PASS");

    return bad ? 1 : 0;
}
