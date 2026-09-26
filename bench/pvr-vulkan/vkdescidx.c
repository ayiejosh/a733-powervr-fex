/* vkdescidx.c - descriptor array indexed by a dynamically-uniform value.
 *
 * The feasibility report's top lead: pco already contains the whole lowering for
 * a runtime descriptor index (O_SMP_DYNIDX / legalize_smp_dynidx, and the
 * generic legalize_dynidx), and the core-1.0 *ArrayDynamicIndexing features are
 * already advertised - but nothing has ever shown that the hardware honours it.
 * If it does not, every plan built on that lowering is built on sand.
 *
 * Two storage buffers are bound as one descriptor array of two. The shader reads
 * b[pc.idx].v with idx pushed as 0 and then 1, and the host checks it got the
 * right buffer each time. Both a sentinel and a per-buffer distinct value are
 * written, so "nothing ran", "it ran and always read descriptor 0", and "it
 * worked" are three distinguishable outcomes.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./vkdescidx
 *
 * Exit status 0 on PASS, 1 on FAIL, 2 on setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "desc_idx_spv.h"

#define DIE(...)                                                                        \
    do {                                                                                \
        fprintf(stderr, "SETUP-FAIL: " __VA_ARGS__);                                    \
        fprintf(stderr, "\n");                                                          \
        exit(2);                                                                        \
    } while (0)

#define VKCHECK(x)                                                                      \
    do {                                                                                \
        VkResult _r = (x);                                                              \
        if (_r != VK_SUCCESS)                                                           \
            DIE("%s -> %d", #x, (int)_r);                                               \
    } while (0)

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

/* One storage buffer of one uint, host visible, filled with `value`. If
 * `mapped_out` is non-NULL the mapping is returned through it - the driver
 * refuses a second vkMapMemory of the same allocation (VK_ERROR_MEMORY_MAP_FAILED),
 * so callers that need the pointer must take it from here. */
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

/* Dispatch once with `idx` pushed, and report what the shader read back. */
static uint32_t dispatch(VkDevice dev, VkQueue queue, VkCommandBuffer cb, VkPipeline pipe,
                         VkPipelineLayout layout, VkDescriptorSet ds, uint32_t *marker_out,
                         uint32_t idx)
{
    const uint32_t pc[2] = { idx, 0xDEADBEEFu };

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

    *marker_out = 0; /* caller re-reads the mapping */
    return 0;
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

    /* The feature this exercises. It is core 1.0 and already advertised, so this
     * is a reminder of which bit the test speaks for, not a gate. */
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(phys, &f);
    check(f.shaderStorageBufferArrayDynamicIndexing,
          "shaderStorageBufferArrayDynamicIndexing = %d", f.shaderStorageBufferArrayDynamicIndexing);

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

    /* The descriptor array: two buffers in one binding. */
    VkDeviceMemory mem_a, mem_b, mem_out;
    void *mapped_out = NULL;
    VkBuffer buf_a = make_buf(dev, phys, &mem_a, 0xAAAA0001u, NULL);
    VkBuffer buf_b = make_buf(dev, phys, &mem_b, 0xBBBB0002u, NULL);
    VkBuffer buf_out = make_buf(dev, phys, &mem_out, 0, &mapped_out);

    VkDescriptorSetLayoutBinding bind[2] = {
        { .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 2, /* the array */
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
                                    .codeSize = sizeof(desc_idx_spv),
                                    .pCode = desc_idx_spv },
                                 NULL, &sm));

    VkPipeline pipe;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1,
                                           &(VkComputePipelineCreateInfo){
                                              .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                         .module = sm,
                                                         .pName = "main" },
                                              .layout = layout },
                                           NULL, &pipe);
    check(pr == VK_SUCCESS, "descriptor-array shader compiled -> %d", (int)pr);
    if (pr != VK_SUCCESS)
        goto report;

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

    uint32_t marker = 0;
    uint32_t got[2] = { 0, 0 };

    for (uint32_t i = 0; i < 2; i++) {
        ((uint32_t *)mapped_out)[0] = 0xFFFFFFFFu; /* so a stale read is obvious */
        ((uint32_t *)mapped_out)[1] = 0;
        dispatch(dev, queue, cb, pipe, layout, ds, &marker, i);
        got[i] = ((uint32_t *)mapped_out)[0];
        marker = ((uint32_t *)mapped_out)[1];
    }

    printf("  pushed idx=0 -> 0x%08x (want 0xaaaa0001), idx=1 -> 0x%08x (want 0xbbbb0002), "
           "marker = 0x%08x (want 0xdeadbeef)\n",
           got[0], got[1], marker);

    check(marker == 0xDEADBEEFu, "the dispatch ran at all (marker 0x%08x)", marker);
    check(got[0] == 0xAAAA0001u, "a uniform index of 0 selected descriptor 0 (0x%08x)", got[0]);
    check(got[1] == 0xBBBB0002u,
          "a uniform index of 1 selected descriptor 1 - the dynamic offset is honoured (0x%08x)",
          got[1]);

report:
    printf("VERDICT: %s (%d ok, %d failed)\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
