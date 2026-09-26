/* vkdescnon.c - non-uniform (per-lane divergent) descriptor indexing.
 *
 * vkdescidx.c proved the dynamically-uniform half: a runtime descriptor index
 * pushed as a constant selects the right buffer, so pco's O_SMP_DYNIDX /
 * legalize_dynidx lowering works on this hardware. The feasibility report left
 * the non-uniform half "undetermined" - and that is the question that decides
 * whether the remaining descriptorIndexing sub-features are a hardware wall or
 * only plumbing.
 *
 * One storage-buffer descriptor array of two, both read by the same shader, in
 * the same pipeline, with the same descriptor set:
 *
 *   mode 0  index = a push constant (dynamically uniform)  -> control
 *   mode 1  index = gl_LocalInvocationID.x & 1             -> adjacent lanes of
 *                                                            one wave disagree
 *
 * The control passing means the descriptor path, the array write and the
 * readback are all sound, so a mode-1 failure isolates divergence handling. The
 * deliberate out-of-spec detail is nonuniformEXT: this driver does not advertise
 * shaderStorageBufferArrayNonUniformIndexing, so the probe is not a conformance
 * claim, it is a capability measurement.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./vkdescnon
 *
 * Exit status 0 on PASS, 1 on FAIL, 2 on setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "desc_nonuniform_spv.h"

#define DIE(...)                             \
    do {                                     \
        fprintf(stderr, "SETUP-FAIL: " __VA_ARGS__); \
        fprintf(stderr, "\n");               \
        exit(2);                             \
    } while (0)

#define VKCHECK(x)                                     \
    do {                                               \
        VkResult _r = (x);                             \
        if (_r != VK_SUCCESS)                          \
            DIE("%s -> %d", #x, (int)_r);              \
    } while (0)

#define LANES 64u
#define VAL_A 0xAAAA0001u
#define VAL_B 0xBBBB0002u

static int pass, fail;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("  %s  ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);

    if (ok)
        pass++;
    else
        fail++;
}

/* One host-visible storage buffer of 4096 bytes, first word set to `value`. */
static VkBuffer make_buf(VkDevice dev, VkPhysicalDevice phys, VkDeviceMemory *mem,
                         uint32_t value, void **mapped_out)
{
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(dev,
                           &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                  .size = 4096,
                                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
                           NULL, &buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mt = i;
            break;
        }
    }

    VKCHECK(vkAllocateMemory(dev,
                             &(VkMemoryAllocateInfo){
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = mt },
                             NULL, mem));
    VKCHECK(vkBindBufferMemory(dev, buf, *mem, 0));

    void *mapped;
    VKCHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, &mapped));
    memset(mapped, 0, 4096);
    ((uint32_t *)mapped)[0] = value;
    if (mapped_out)
        *mapped_out = mapped;

    return buf;
}

/* One dispatch, then copy the LANES output words out for checking. */
static void run(VkDevice dev, VkQueue queue, VkCommandBuffer cb, VkPipeline pipe,
                VkPipelineLayout layout, VkDescriptorSet ds, VkDeviceMemory mem_out,
                void *mapped, uint32_t mode, uint32_t uniform_idx, uint32_t *out)
{
    const uint32_t pc[2] = { mode, uniform_idx };

    VKCHECK(vkResetCommandBuffer(cb, 0));
    VKCHECK(vkBeginCommandBuffer(cb,
                                 &(VkCommandBufferBeginInfo){
                                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cb, 1, 1, 1);
    VKCHECK(vkEndCommandBuffer(cb));

    VkFence fence;
    VKCHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO },
                          NULL, &fence));
    VKCHECK(vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                           .commandBufferCount = 1,
                                           .pCommandBuffers = &cb },
                          fence));
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
    VKCHECK(vkQueueWaitIdle(queue));
    vkDestroyFence(dev, fence, NULL);

    /* The output was written by the GPU: invalidate so the CPU sees it rather
     * than a stale cache line (the driver implements this through
     * DMA_BUF_IOCTL_SYNC on the BO's dma-buf). */
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = mem_out,
                              .offset = 0,
                              .size = VK_WHOLE_SIZE };
    vkInvalidateMappedMemoryRanges(dev, 1, &r);

    memcpy(out, mapped, LANES * sizeof(uint32_t));
}

int main(void)
{
    VkInstance instance;
    VKCHECK(vkCreateInstance(&(VkInstanceCreateInfo){
                                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &(VkApplicationInfo){
                                   .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_2 } },
                             NULL, &instance));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    VkPhysicalDevice phys;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, &phys));

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    /* Which bit this probe speaks for. Not a gate: the whole point is to measure
     * beyond it. */
    VkPhysicalDeviceDescriptorIndexingFeatures di = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                     .pNext = &di };
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    printf("advertised: descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing = %d, "
           "core.shaderStorageBufferArrayDynamicIndexing = %d\n",
           di.shaderStorageBufferArrayNonUniformIndexing,
           f2.features.shaderStorageBufferArrayDynamicIndexing);

    float prio = 1.0f;
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys,
                           &(VkDeviceCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                 .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueCount = 1,
                                 .pQueuePriorities = &prio } },
                           NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev,
                                &(VkCommandPoolCreateInfo){
                                   .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO },
                                NULL, &pool));

    VkDeviceMemory mem_a, mem_b, mem_out;
    void *mapped_out = NULL;
    VkBuffer buf_a = make_buf(dev, phys, &mem_a, VAL_A, NULL);
    VkBuffer buf_b = make_buf(dev, phys, &mem_b, VAL_B, NULL);
    VkBuffer buf_out = make_buf(dev, phys, &mem_out, 0, &mapped_out);

    VkDescriptorSetLayoutBinding bind[2] = {
        { .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 2,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev,
                                        &(VkDescriptorSetLayoutCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2,
                                           .pBindings = bind },
                                        NULL, &dsl));
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(dev,
                                   &(VkPipelineLayoutCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &(VkPushConstantRange){
                                         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                         .size = 8 } },
                                   NULL, &layout));

    VkShaderModule sm;
    VKCHECK(vkCreateShaderModule(dev,
                                 &(VkShaderModuleCreateInfo){
                                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(desc_nonuniform_spv),
                                    .pCode = desc_nonuniform_spv },
                                 NULL, &sm));

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1,
                                           &(VkComputePipelineCreateInfo){
                                              .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                         .module = sm,
                                                         .pName = "main" },
                                              .layout = layout },
                                           NULL, &pipe);
    check(pr == VK_SUCCESS, "the non-uniform-index shader compiled -> %d", (int)pr);
    if (pr != VK_SUCCESS) {
        printf("VERDICT: FAIL (%d ok, %d failed) - the compiler rejects a divergent "
               "descriptor index before any hardware question is reached\n", pass, fail);
        return 1;
    }

    VkDescriptorPool dp;
    VKCHECK(vkCreateDescriptorPool(dev,
                                   &(VkDescriptorPoolCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &(VkDescriptorPoolSize){
                                         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .descriptorCount = 3 } },
                                   NULL, &dp));
    VkDescriptorSet ds;
    VKCHECK(vkAllocateDescriptorSets(dev,
                                     &(VkDescriptorSetAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &dsl },
                                     &ds));

    VkDescriptorBufferInfo bi[3] = {
        { .buffer = buf_a, .range = VK_WHOLE_SIZE },
        { .buffer = buf_b, .range = VK_WHOLE_SIZE },
        { .buffer = buf_out, .range = VK_WHOLE_SIZE },
    };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 0,
          .descriptorCount = 2,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &bi[2] },
    };
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);

    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(dev,
                                     &(VkCommandBufferAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 },
                                     &cb));

    uint32_t got[LANES];

    /* ---- control: dynamically uniform index, one dispatch per descriptor ---- */
    for (uint32_t idx = 0; idx < 2; idx++) {
        const uint32_t want = idx == 0 ? VAL_A : VAL_B;
        uint32_t wrong = 0;

        for (uint32_t i = 0; i < LANES; i++)
            ((uint32_t *)mapped_out)[i] = 0xFFFFFFFFu;

        run(dev, queue, cb, pipe, layout, ds, mem_out, mapped_out, 0, idx, got);

        for (uint32_t i = 0; i < LANES; i++)
            if (got[i] != want)
                wrong++;

        check(wrong == 0, "uniform index %u selected descriptor %u for every lane "
                          "(%u/%u wrong, first word 0x%08x)",
              idx, idx, wrong, LANES, got[0]);
    }

    /* ---- the actual question: the index diverges inside the wave ---- */
    uint32_t wrong = 0, first_bad = 0, wrong_even = 0, wrong_odd = 0;
    for (uint32_t i = 0; i < LANES; i++)
        ((uint32_t *)mapped_out)[i] = 0xFFFFFFFFu;

    run(dev, queue, cb, pipe, layout, ds, mem_out, mapped_out, 1, 0, got);

    for (uint32_t i = 0; i < LANES; i++) {
        const uint32_t want = (i & 1u) ? VAL_B : VAL_A;
        if (got[i] != want) {
            if (wrong == 0)
                first_bad = i;
            if (i & 1u)
                wrong_odd++;
            else
                wrong_even++;
            wrong++;
        }
    }

    /* Which descriptor the lanes actually landed on says which failure this is:
     * every lane reading the same buffer means the index was treated as uniform
     * (the value was hoisted out of the divergent branch), while a mixed but wrong
     * pattern means the address computation itself is wrong. */
    uint32_t n_a = 0, n_b = 0, n_other = 0;
    for (uint32_t i = 0; i < LANES; i++) {
        if (got[i] == VAL_A)
            n_a++;
        else if (got[i] == VAL_B)
            n_b++;
        else
            n_other++;
    }

    printf("  divergent index (lane & 1): %u/%u lanes wrong "
           "(%u even lanes, %u odd lanes, first bad lane %u)\n",
           wrong, LANES, wrong_even, wrong_odd, first_bad);
    printf("    lanes that read descriptor 0: %u, descriptor 1: %u, neither: %u\n",
           n_a, n_b, n_other);
    if (wrong) {
        printf("    lanes 0-7: ");
        for (uint32_t i = 0; i < 8; i++)
            printf("0x%08x ", got[i]);
        printf("\n");
        printf("    (%s)\n",
               (n_a == LANES || n_b == LANES)
                  ? "every lane read the same descriptor: the index was treated as uniform"
                  : "mixed wrong values: the per-lane address computation is wrong");
    }
    check(wrong == 0, "every lane selected its own descriptor");

    printf("VERDICT: %s (%d ok, %d failed)%s\n", fail ? "FAIL" : "PASS", pass, fail,
           fail ? "" : " - non-uniform descriptor indexing is reachable on this hardware");
    return fail ? 1 : 0;
}
