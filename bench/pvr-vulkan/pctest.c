/* pctest.c - minimal vkCmdPushConstants probe.
 *
 * Push constants are the mechanism an application uses to hand small values to a
 * shader, and it is also how applications deliver buffer device addresses. Found
 * broken while testing buffer device addresses: a shader that received its
 * address this way read zeros, while the identical shader receiving the same
 * address through a uniform buffer worked.
 *
 * This test has no buffer device addresses in it at all, so the result is about
 * push constants and nothing else. It distinguishes the three failure modes that
 * would otherwise look alike from the outside:
 *
 *   - nothing lands in the output buffer  -> the dispatch never ran, or the
 *     storage buffer path is broken (not a push constant bug);
 *   - the marker lands but the values are zero -> the dispatch ran and read a
 *     zeroed or entirely absent push constant buffer;
 *   - only some of the values land -> the partial-update bookkeeping is wrong
 *     (which range was pushed, and at what offset).
 *
 * The shader writes a fixed marker alongside the copied values for exactly that
 * reason.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./pctest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "pc_spv.h"
#include "pc64_spv.h"

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

static int fails;

static uint32_t pick_memory(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

struct env {
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkPipeline pipe;      /* push constant block declared as uvec4 */
    VkPipeline pipe_u64;  /* push constant block declared as two uint64_t */
    VkPipelineLayout layout;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDescriptorSet set;
    void *map;
    PFN_vkCmdPushConstants push;
};

/* push <offset> <dwords...> then dispatch once and report the 8 dwords back. */
static void probe(struct env *e, const char *name, int u64, uint32_t offset,
                  const uint32_t *vals, uint32_t nvals, const uint32_t expect[8])
{
    uint32_t *host = e->map;
    for (unsigned i = 0; i < 8; i++)
        host[i] = 0;

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

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, u64 ? e->pipe_u64 : e->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, e->layout, 0, 1, &e->set, 0, NULL);
    e->push(cb, e->layout, VK_SHADER_STAGE_COMPUTE_BIT, offset, nvals * 4, vals);
    vkCmdDispatch(cb, 1, 1, 1);

    VkBufferMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .buffer = e->buf,
                                  .offset = 0,
                                  .size = 32 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bar, 0, NULL);
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
        DIE("%s: vkWaitForFences -> %d", name, (int)w);

    int ok = 1, marker_ok = (host[4] == 0xDEADBEEFu && host[5] == 1u && host[6] == 2u && host[7] == 3u);
    for (unsigned i = 0; i < 8; i++)
        if (host[i] != expect[i])
            ok = 0;

    printf("  %-9s %-28s %s   got %08x %08x %08x %08x | marker %s\n", u64 ? "uint64_t" : "uvec4",
           name, ok ? "PASS" : "FAIL", host[0], host[1], host[2], host[3],
           marker_ok ? "ok" : "MISSING (dispatch did not run)");
    if (!ok) {
        printf("  %-9s %-28s want %08x %08x %08x %08x\n", "", "", expect[0], expect[1], expect[2],
               expect[3]);
        fails++;
    }

    vkDestroyFence(e->dev, fence, NULL);
    vkFreeCommandBuffers(e->dev, e->pool, 1, &cb);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "pvr-pc-test",
                              .apiVersion = VK_API_VERSION_1_1 };
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
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys,
                           &(VkDeviceCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                   .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = qfi,
                                   .queueCount = 1,
                                   .pQueuePriorities = &prio } },
                           NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    PFN_vkCmdPushConstants push =
        (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(dev, "vkCmdPushConstants");
    printf("vkCmdPushConstants = %s\n", push ? "resolved" : "NULL");
    if (!push)
        DIE("no vkCmdPushConstants");

    /* ---- output buffer -------------------------------------------------- */
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(dev,
                           &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                  .size = 32,
                                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE },
                           NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t mti = pick_memory(phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mti == UINT32_MAX)
        DIE("no host visible memory type");
    VkDeviceMemory mem;
    VKCHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                           .allocationSize = mr.size,
                                                           .memoryTypeIndex = mti },
                             NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));

    /* ---- pipeline ------------------------------------------------------- */
    VkDescriptorSetLayoutBinding dslb = { .binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
                                                 .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 1,
                                                 .pBindings = &dslb },
                                        NULL, &dsl));
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16 };
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
                                             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .setLayoutCount = 1,
                                             .pSetLayouts = &dsl,
                                             .pushConstantRangeCount = 1,
                                             .pPushConstantRanges = &pcr },
                                   NULL, &layout));

    VkShaderModule sm, sm64;
    VKCHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                           .codeSize = sizeof(pc_spv),
                                           .pCode = pc_spv },
                                 NULL, &sm));
    VKCHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                           .codeSize = sizeof(pc64_spv),
                                           .pCode = pc64_spv },
                                 NULL, &sm64));
    VkPipeline pipe, pipe_u64;
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .pName = "main" },
        .layout = layout,
    };
    cpci.stage.module = sm;
    VKCHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    cpci.stage.module = sm64;
    VKCHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe_u64));

    VkDescriptorPool dpool;
    VKCHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
                                            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1,
                                            .poolSizeCount = 1,
                                            .pPoolSizes = &(VkDescriptorPoolSize){
                                                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                .descriptorCount = 1 } },
                                   NULL, &dpool));
    VkDescriptorSet set;
    VKCHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
                                              .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = dpool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &dsl },
                                     &set));
    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = 32 };
    vkUpdateDescriptorSets(dev, 1,
                           &(VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                    .dstSet = set,
                                                    .dstBinding = 0,
                                                    .descriptorCount = 1,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    .pBufferInfo = &dbi },
                           0, NULL);

    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
                                          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .queueFamilyIndex = qfi },
                                NULL, &pool));

    struct env e = { .dev = dev, .queue = queue, .pool = pool, .pipe = pipe, .pipe_u64 = pipe_u64,
                     .layout = layout, .buf = buf, .mem = mem, .set = set, .push = push };
    VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &e.map));

    const uint32_t a[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    const uint32_t one[1] = { 0xAAAAAAAAu };
    const uint32_t hi[2] = { 0xBBBBBBBBu, 0xCCCCCCCCu };

    printf("\n== push constant probes (shader copies the block to an SSBO) ==\n");
    printf("  (identical bytes are pushed for both block declarations)\n");
    const uint32_t exp_full[8] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                   0xDEADBEEFu, 1u, 2u, 3u };
    const uint32_t exp_one[8] = { 0xAAAAAAAAu, 0, 0, 0, 0xDEADBEEFu, 1u, 2u, 3u };
    const uint32_t exp_hi[8] = { 0, 0, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDEADBEEFu, 1u, 2u, 3u };
    for (int u64 = 0; u64 < 2; u64++) {
        probe(&e, "full 16 bytes at offset 0", u64, 0, a, 4, exp_full);
        probe(&e, "4 bytes at offset 0", u64, 0, one, 1, exp_one);
        probe(&e, "8 bytes at offset 8", u64, 8, hi, 2, exp_hi);
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");

    vkUnmapMemory(dev, mem);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipeline(dev, pipe_u64, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyShaderModule(dev, sm64, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);

    return fails ? 1 : 0;
}
