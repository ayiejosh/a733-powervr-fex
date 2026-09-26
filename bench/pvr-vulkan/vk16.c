/* vk16.c - shaderFloat16 / shaderInt8 arithmetic probe.
 *
 * The narrow-type *storage* features need memory-access lowering; this is the
 * other half of the group, and it is testable on its own: does the backend run
 * 8/16-bit arithmetic correctly when the device features are enabled?
 *
 *   VK_ICD_FILENAMES=<icd.json> ./vk16
 *
 * Exit status 0 on PASS, 1 on FAIL, 2 on setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "f16_alu_spv.h"

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

    VkPhysicalDevice16BitStorageFeatures s16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    VkPhysicalDeviceShaderFloat16Int8Features f16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
        .pNext = &s16 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                     .pNext = &f16 };
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    check(f16.shaderFloat16, "device feature shaderFloat16 = %d", f16.shaderFloat16);
    check(f16.shaderInt8, "device feature shaderInt8 = %d", f16.shaderInt8);

    float prio = 1.0f;
    VkDevice dev;
    VkResult dr = vkCreateDevice(phys,
                                 &(VkDeviceCreateInfo){
                                    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &f16,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                       .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueCount = 1,
                                       .pQueuePriorities = &prio } },
                                 NULL, &dev);
    check(dr == VK_SUCCESS, "vkCreateDevice with shaderFloat16 + shaderInt8 -> %d", (int)dr);
    if (dr != VK_SUCCESS)
        goto report;

    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev,
                                &(VkCommandPoolCreateInfo){
                                   .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO },
                                NULL, &pool));

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

    VkDeviceMemory mem;
    VKCHECK(vkAllocateMemory(dev,
                             &(VkMemoryAllocateInfo){
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = mt },
                             NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));

    void *mapped;
    VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
    memset(mapped, 0, 4096);

    VkDescriptorSetLayoutBinding bind = { .binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev,
                                        &(VkDescriptorSetLayoutCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1,
                                           .pBindings = &bind },
                                        NULL, &dsl));
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(dev,
                                   &(VkPipelineLayoutCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &dsl },
                                   NULL, &layout));

    VkShaderModule sm;
    VKCHECK(vkCreateShaderModule(dev,
                                 &(VkShaderModuleCreateInfo){
                                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(f16_alu_spv),
                                    .pCode = f16_alu_spv },
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
    check(pr == VK_SUCCESS, "narrow-type compute shader compiled -> %d", (int)pr);
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
                                         .descriptorCount = 1 } },
                                   NULL, &dp));
    VkDescriptorSet ds;
    VKCHECK(vkAllocateDescriptorSets(dev,
                                     &(VkDescriptorSetAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &dsl },
                                     &ds));
    vkUpdateDescriptorSets(dev, 1,
                           &(VkWriteDescriptorSet){
                              .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = ds,
                              .dstBinding = 0,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .pBufferInfo = &(VkDescriptorBufferInfo){ .buffer = buf,
                                                                        .range = VK_WHOLE_SIZE } },
                           0, NULL);

    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(dev,
                                     &(VkCommandBufferAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 },
                                     &cb));
    VKCHECK(vkBeginCommandBuffer(cb,
                                 &(VkCommandBufferBeginInfo){
                                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
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

    uint32_t *v = mapped;
    printf("  results: f16 1.5*2.25*8 = %u (want 27), i8 -100/3+128 = %u (want 95), "
           "u8 200+100 = %u (want 44), f16 cmp = %u (want 1), untouched[6] = %u\n",
           v[0], v[1], v[2], v[3], v[6]);

    check(v[0] == 27, "f16 multiply is exact (%u)", v[0]);
    check(v[1] == 95, "int8 division truncates and the value survives (%u)", v[1]);
    check(v[2] == 44, "uint8 addition wraps (%u)", v[2]);
    check(v[3] == 1, "f16 comparison works (%u)", v[3]);
    check(v[6] == 0, "the write stayed inside its slot (v[6]=%u)", v[6]);

    /* What is NOT covered: per-operation f16 rounding. A probe summing
     * 1.0 + 0.0005 twice returns the same constant-folded value on this driver
     * and on the vendor, so it distinguishes nothing. */
    printf("  note: per-operation f16 rounding is not covered by this test\n");

report:
    printf("VERDICT: %s (%d ok, %d failed)\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
