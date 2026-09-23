/* bda.c - VK_KHR_buffer_device_address functional test.
 *
 * The driver already had vkGetBufferDeviceAddress(), the compiler already had a
 * 64-bit global address path, and descriptors already carry a buffer's device VA
 * (pvr_arch_descriptor_set.c fills them with PVR_DEV_ADDR_OFFSET(buffer->dev_addr)).
 * What was missing was the feature/extension advertisement, so none of it was
 * reachable. This test answers the question that has to be answered before
 * advertising it: does a shader handed a raw device address actually read and
 * write that buffer?
 *
 * It runs a matrix rather than a single case, because "nothing was written" has
 * several possible causes and they need to be told apart:
 *
 *   address source    push constant (uint64_t, the way apps do it) vs
 *                     uniform buffer (two 32-bit halves packed in the shader)
 *   access mode       constant store (needs no prior content), load+store,
 *                     global atomic
 *
 * A constant store that lands proves the address and the element indexing; a
 * load+store that then fails points at the read path; a push-constant-only
 * failure points at delivery rather than at the address itself.
 *
 * Method notes:
 *   - The buffer under test is never mapped by the CPU; every transfer goes
 *     through a staging buffer with vkCmdCopyBuffer. That keeps the result
 *     independent of the driver's host-visible memory caching (a known separate
 *     limitation), so a failure here means the address is wrong, not that a
 *     cache was stale.
 *   - Every element gets a distinct expected value, not a constant, so an
 *     address that is off by a few bytes or that aliases another element cannot
 *     pass.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./bda
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "bda_pc_spv.h"
#include "bda_ubo_spv.h"

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

#define MODE_RMW 0u
#define MODE_ATOMIC 1u
#define MODE_CONST 2u

/* Soft failures: report every problem instead of stopping at the first. */
static int fails;

static void check(int ok, const char *what)
{
    printf("  %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        fails++;
}

static uint32_t pick_memory(VkPhysicalDevice phys,
                            uint32_t bits,
                            VkMemoryPropertyFlags want,
                            VkMemoryPropertyFlags avoid)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)))
                continue;
            if ((f & want) != want)
                continue;
            if (pass == 0 && (f & avoid))
                continue;
            return i;
        }
    }
    return UINT32_MAX;
}

struct pc_block {
    uint64_t addr;
    uint32_t count;
    uint32_t mode;
};

struct env {
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkBuffer buf_a;   /* buffer under test */
    VkBuffer buf_b;   /* host-visible staging */
    VkDeviceMemory mem_b;
    VkDeviceSize bytes;
    uint64_t addr_a;
    uint32_t count;
    void *map_b;
    void *map_p;

    VkPipeline pipe_pc;
    VkPipelineLayout layout_pc;
    VkPipeline pipe_ubo;
    VkPipelineLayout layout_ubo;
    PFN_vkCmdPushConstants push_constants;
    VkDescriptorSet set_ubo;

    struct pc_block pc_staging;
};

/* One probe: fill staging, run the shader, copy back, verify. */
static void run_probe(struct env *e, const char *name, int use_pc, uint32_t mode)
{
    uint32_t *host = e->map_b;
    for (uint32_t i = 0; i < e->count; i++)
        host[i] = 100u + i;

    if (use_pc) {
        struct pc_block pc = { .addr = e->addr_a, .count = e->count, .mode = mode };
        /* recorded inside the command buffer below */
        e->pc_staging = pc;
    } else {
        uint32_t *p = e->map_p;
        p[0] = (uint32_t)(e->addr_a & 0xffffffffu);
        p[1] = (uint32_t)(e->addr_a >> 32);
        p[2] = e->count;
        p[3] = mode;
    }

    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(e->dev,
                                     &(VkCommandBufferAllocateInfo){
                                         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = e->pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 },
                                     &cb));
    VKCHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
                                         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));

    VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = e->bytes };
    vkCmdCopyBuffer(cb, e->buf_b, e->buf_a, 1, &region);

    VkBufferMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .buffer = e->buf_a,
                                  .offset = 0,
                                  .size = e->bytes };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 1, &bar, 0, NULL);

    if (use_pc) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, e->pipe_pc);
        e->push_constants(cb, e->layout_pc, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(e->pc_staging), &e->pc_staging);
    } else {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, e->pipe_ubo);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, e->layout_ubo, 0, 1,
                                &e->set_ubo, 0, NULL);
    }

    vkCmdDispatch(cb, (e->count + 63u) / 64u, 1, 1);

    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bar, 0, NULL);

    vkCmdCopyBuffer(cb, e->buf_a, e->buf_b, 1, &region);

    VkBufferMemoryBarrier host_bar = bar;
    host_bar.buffer = e->buf_b;
    host_bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_bar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_bar, 0, NULL);

    VKCHECK(vkEndCommandBuffer(cb));

    VkFence fence;
    VKCHECK(vkCreateFence(e->dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO },
                          NULL, &fence));

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &cb };
    VKCHECK(vkQueueSubmit(e->queue, 1, &si, fence));
    VkResult w = vkWaitForFences(e->dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (w != VK_SUCCESS)
        DIE("%s: vkWaitForFences -> %d (GPU never signalled)", name, (int)w);

    uint32_t bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < e->count; i++) {
        uint32_t want = (mode == MODE_CONST) ? (0xA5A50000u | i) : (101u + i);
        if (host[i] != want) {
            if (!bad)
                first_bad = i;
            bad++;
        }
    }

    printf("  %-22s %s: %3u/%u correct", use_pc ? "push-constant" : "uniform-buffer", name,
           e->count - bad, e->count);
    if (bad) {
        uint32_t want = (mode == MODE_CONST) ? (0xA5A50000u | first_bad) : (101u + first_bad);
        printf("   first bad [%u] got 0x%08x want 0x%08x", first_bad, host[first_bad], want);
    }
    printf("   raw [0]=0x%08x [1]=0x%08x [255]=0x%08x\n", host[0], host[1], host[255]);

    if (bad)
        fails++;

    vkDestroyFence(e->dev, fence, NULL);
    vkFreeCommandBuffers(e->dev, e->pool, 1, &cb);
}

int main(int argc, char **argv)
{
    /* --bda-only skips the push-constant probes. They cover how an application
     * delivers an address, but they depend on a separate, open driver bug in
     * 64-bit push constant handling, so this flag gives the buffer device
     * address result a verdict of its own. */
    const int bda_only = (argc > 1 && strcmp(argv[1], "--bda-only") == 0);

    /* Unbuffered: a crash part-way through must not cost us the diagnosis, and
     * stdout is a pipe under most runners. */
    setvbuf(stdout, NULL, _IONBF, 0);

    const uint32_t count = 256;
    const VkDeviceSize bytes = count * sizeof(uint32_t);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "pvr-bda-test",
                              .apiVersion = VK_API_VERSION_1_2 };
    VkInstance instance;
    VKCHECK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                      .pApplicationInfo = &app },
                             NULL, &instance));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (!ndev)
        DIE("no Vulkan physical device (is VK_ICD_FILENAMES set?)");
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));
    VkPhysicalDevice phys = devs[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    /* ---- what does the driver claim? ------------------------------------ */
    printf("\n== buffer device address support ==\n");
    {
        uint32_t n = 0;
        VKCHECK(vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL));
        VkExtensionProperties *e = calloc(n, sizeof(*e));
        VKCHECK(vkEnumerateDeviceExtensionProperties(phys, NULL, &n, e));
        int khr = 0, ext = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!strcmp(e[i].extensionName, "VK_KHR_buffer_device_address"))
                khr = 1;
            if (!strcmp(e[i].extensionName, "VK_EXT_buffer_device_address"))
                ext = 1;
        }
        check(khr, "extension VK_KHR_buffer_device_address");
        check(ext, "extension VK_EXT_buffer_device_address");
        free(e);
    }

    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures fbda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        .pNext = &f12,
    };
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT fbda_ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT,
        .pNext = &fbda,
    };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                     .pNext = &fbda_ext };
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    printf("  Vulkan12Features.bufferDeviceAddress    = %d (captureReplay=%d multiDevice=%d)\n",
           f12.bufferDeviceAddress, f12.bufferDeviceAddressCaptureReplay, f12.bufferDeviceAddressMultiDevice);
    printf("  BufferDeviceAddressFeatures.bufferDeviceAddress = %d (captureReplay=%d)\n",
           fbda.bufferDeviceAddress, fbda.bufferDeviceAddressCaptureReplay);
    printf("  ...FeaturesEXT.bufferDeviceAddress      = %d\n", fbda_ext.bufferDeviceAddress);
    check(f12.bufferDeviceAddress, "Vulkan12Features.bufferDeviceAddress");

    /* ---- device --------------------------------------------------------- */
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qfi == UINT32_MAX)
            qfi = i;
    if (qfi == UINT32_MAX)
        DIE("no compute queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qfi,
                                    .queueCount = 1,
                                    .pQueuePriorities = &prio };
    /* Ask for the feature through the core 1.2 struct, the way an application
     * targeting Vulkan 1.2 does, and enable the extension so the KHR entry
     * point names are legal to call as well. */
    VkPhysicalDeviceVulkan12Features enable12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    const char *dev_ext[] = { "VK_KHR_buffer_device_address" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = &enable12,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1,
                               .ppEnabledExtensionNames = dev_ext };
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys, &dci, NULL, &dev));

    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    /* ---- entry point names ---------------------------------------------- */
    printf("\n== entry points (vkGetDeviceProcAddr) ==\n");
    PFN_vkGetBufferDeviceAddress p_core =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddress");
    PFN_vkGetBufferDeviceAddressKHR p_khr =
        (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddressKHR");
    PFN_vkGetBufferDeviceAddressEXT p_ext =
        (PFN_vkGetBufferDeviceAddressEXT)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddressEXT");
    printf("  vkGetBufferDeviceAddress    = %s\n", p_core ? "resolved" : "NULL");
    printf("  vkGetBufferDeviceAddressKHR = %s\n", p_khr ? "resolved" : "NULL");
    printf("  vkGetBufferDeviceAddressEXT = %s\n", p_ext ? "resolved" : "NULL");
    check(p_core != NULL, "core entry point resolves");
    if (!p_core)
        DIE("cannot continue without vkGetBufferDeviceAddress");

    PFN_vkCmdPushConstants p_push =
        (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(dev, "vkCmdPushConstants");
    printf("  vkCmdPushConstants           = %s\n", p_push ? "resolved" : "NULL");
    check(p_push != NULL, "vkCmdPushConstants resolves");
    if (!p_push)
        DIE("cannot continue without vkCmdPushConstants");

    /* ---- buffers -------------------------------------------------------- */
    VkBufferCreateInfo bci_a = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf_a, buf_b, buf_p;
    VKCHECK(vkCreateBuffer(dev, &bci_a, NULL, &buf_a));

    VkBufferCreateInfo bci_b = bci_a;
    bci_b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VKCHECK(vkCreateBuffer(dev, &bci_b, NULL, &buf_b));

    VkBufferCreateInfo bci_p = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = 16,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VKCHECK(vkCreateBuffer(dev, &bci_p, NULL, &buf_p));

    VkMemoryRequirements mr_a, mr_b, mr_p;
    vkGetBufferMemoryRequirements(dev, buf_a, &mr_a);
    vkGetBufferMemoryRequirements(dev, buf_b, &mr_b);
    vkGetBufferMemoryRequirements(dev, buf_p, &mr_p);

    /* The buffer under test prefers real device memory; staging and parameters
     * are host visible. Both fall back so the test also runs on a driver that
     * only exposes host-visible heaps. */
    uint32_t mti_a = pick_memory(phys, mr_a.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mti_a == UINT32_MAX)
        mti_a = pick_memory(phys, mr_a.memoryTypeBits, 0, 0);
    uint32_t mti_b = pick_memory(phys, mr_b.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    uint32_t mti_p = pick_memory(phys, mr_p.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (mti_a == UINT32_MAX || mti_b == UINT32_MAX || mti_p == UINT32_MAX)
        DIE("no usable memory type (a=%u b=%u p=%u)", mti_a, mti_b, mti_p);

    VkDeviceMemory mem_a, mem_b, mem_p;
    VKCHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                           .allocationSize = mr_a.size,
                                                           .memoryTypeIndex = mti_a },
                             NULL, &mem_a));
    VKCHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                           .allocationSize = mr_b.size,
                                                           .memoryTypeIndex = mti_b },
                             NULL, &mem_b));
    VKCHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                           .allocationSize = mr_p.size,
                                                           .memoryTypeIndex = mti_p },
                             NULL, &mem_p));
    VKCHECK(vkBindBufferMemory(dev, buf_a, mem_a, 0));
    VKCHECK(vkBindBufferMemory(dev, buf_b, mem_b, 0));
    VKCHECK(vkBindBufferMemory(dev, buf_p, mem_p, 0));

    /* ---- the addresses -------------------------------------------------- */
    printf("\n== addresses ==\n");
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf_a };
    uint64_t addr_a = p_core(dev, &ai);
    uint64_t addr_a2 = p_core(dev, &ai);
    uint64_t addr_b = p_core(dev, &(VkBufferDeviceAddressInfo){
                                         .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf_b });

    printf("  A = 0x%012llx   (mem type %u, %llu bytes)\n", (unsigned long long)addr_a, mti_a,
           (unsigned long long)bytes);
    printf("  B = 0x%012llx   (mem type %u)\n", (unsigned long long)addr_b, mti_b);
    check(addr_a != 0, "address is non-zero");
    check(addr_a != addr_b, "distinct buffers get distinct addresses");
    check(addr_a == addr_a2, "address is stable across calls");
    check((addr_a & 3u) == 0, "address is 4-byte aligned (uint element)");
    check((addr_a % mr_a.alignment) == 0, "address honours VkMemoryRequirements::alignment");

    if (p_khr)
        check(p_khr(dev, &ai) == addr_a, "KHR entry point agrees with core");
    if (p_ext)
        check(p_ext(dev, &ai) == addr_a, "EXT entry point agrees with core");

    /* ---- pipelines ------------------------------------------------------ */
    printf("\n== pipelines ==\n");
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16 };
    VkPipelineLayout layout_pc;
    VKCHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
                                             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .pushConstantRangeCount = 1,
                                             .pPushConstantRanges = &pcr },
                                   NULL, &layout_pc));

    VkDescriptorSetLayoutBinding dslb = { .binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
                                                 .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 1,
                                                 .pBindings = &dslb },
                                        NULL, &dsl));
    VkPipelineLayout layout_ubo;
    VKCHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
                                             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .setLayoutCount = 1,
                                             .pSetLayouts = &dsl },
                                   NULL, &layout_ubo));

    VkShaderModule sm_pc, sm_ubo;
    VKCHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                           .codeSize = sizeof(bda_pc_spv),
                                           .pCode = bda_pc_spv },
                                 NULL, &sm_pc));
    VKCHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                           .codeSize = sizeof(bda_ubo_spv),
                                           .pCode = bda_ubo_spv },
                                 NULL, &sm_ubo));

    VkPipeline pipe_pc, pipe_ubo;
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .pName = "main" },
    };

    cpci.stage.module = sm_pc;
    cpci.layout = layout_pc;
    printf("  creating push-constant pipeline...\n");
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe_pc);
    if (pr != VK_SUCCESS)
        DIE("vkCreateComputePipelines(push constant) -> %d", (int)pr);

    cpci.stage.module = sm_ubo;
    cpci.layout = layout_ubo;
    printf("  creating uniform-buffer pipeline...\n");
    pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe_ubo);
    if (pr != VK_SUCCESS)
        DIE("vkCreateComputePipelines(uniform buffer) -> %d", (int)pr);
    printf("  both pipelines created\n");

    /* ---- descriptors ---------------------------------------------------- */
    VkDescriptorPool dpool;
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 };
    VKCHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
                                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1,
                                            .poolSizeCount = 1,
                                            .pPoolSizes = &dps },
                                   NULL, &dpool));
    VkDescriptorSet set_ubo;
    VKCHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
                                              .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = dpool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &dsl },
                                     &set_ubo));
    VkDescriptorBufferInfo dbi = { .buffer = buf_p, .offset = 0, .range = 16 };
    vkUpdateDescriptorSets(dev, 1,
                           &(VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                    .dstSet = set_ubo,
                                                    .dstBinding = 0,
                                                    .descriptorCount = 1,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                    .pBufferInfo = &dbi },
                           0, NULL);

    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
                                          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .queueFamilyIndex = qfi },
                                NULL, &pool));

    struct env e = { .dev = dev,
                     .queue = queue,
                     .pool = pool,
                     .buf_a = buf_a,
                     .buf_b = buf_b,
                     .mem_b = mem_b,
                     .bytes = bytes,
                     .addr_a = addr_a,
                     .count = count,
                     .pipe_pc = pipe_pc,
                     .layout_pc = layout_pc,
                     .pipe_ubo = pipe_ubo,
                     .layout_ubo = layout_ubo,
                     .push_constants = p_push,
                     .set_ubo = set_ubo };
    VKCHECK(vkMapMemory(dev, mem_b, 0, VK_WHOLE_SIZE, 0, &e.map_b));
    VKCHECK(vkMapMemory(dev, mem_p, 0, VK_WHOLE_SIZE, 0, &e.map_p));

    /* ---- the actual question -------------------------------------------- */
    printf("\n== shader access through the raw device address ==\n");
    printf("  (constant store needs no prior content; rmw/atomic expect staged 100+i -> 101+i)\n");
    if (!bda_only) {
        run_probe(&e, "const store", 1, MODE_CONST);
        run_probe(&e, "load+store", 1, MODE_RMW);
        run_probe(&e, "atomicAdd", 1, MODE_ATOMIC);
    } else {
        printf("  push-constant probes skipped (--bda-only)\n");
    }
    run_probe(&e, "const store", 0, MODE_CONST);
    run_probe(&e, "load+store", 0, MODE_RMW);
    run_probe(&e, "atomicAdd", 0, MODE_ATOMIC);

    printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");

    vkUnmapMemory(dev, mem_b);
    vkUnmapMemory(dev, mem_p);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyPipeline(dev, pipe_pc, NULL);
    vkDestroyPipeline(dev, pipe_ubo, NULL);
    vkDestroyPipelineLayout(dev, layout_pc, NULL);
    vkDestroyPipelineLayout(dev, layout_ubo, NULL);
    vkDestroyShaderModule(dev, sm_pc, NULL);
    vkDestroyShaderModule(dev, sm_ubo, NULL);
    vkDestroyBuffer(dev, buf_a, NULL);
    vkDestroyBuffer(dev, buf_b, NULL);
    vkDestroyBuffer(dev, buf_p, NULL);
    vkFreeMemory(dev, mem_a, NULL);
    vkFreeMemory(dev, mem_b, NULL);
    vkFreeMemory(dev, mem_p, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);

    return fails ? 1 : 0;
}
