/* cstp - compute throughput probe: is the open driver's deficit graphics-specific?
 *
 * Dispatches W workgroups of 64 invocations, each doing a few dependent ALU ops and one
 * store, and reports invocations/s. Same shape as vkrender's fill, so the two can be
 * compared directly on the same driver: if compute is also ~2.4x down the deficit is
 * global (submission/serialization); if compute is fine it is the raster/PBE path.
 *
 * usage: ./cstp <workgroups> <iters>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "cstpf4_comp_spv.h"

#define CK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",#x,r_);exit(1);} } while(0)
#define DIE(...) do { fprintf(stderr,"FAIL: "); fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); exit(1); } while(0)

VkInstance inst; VkDevice dev; VkDescriptorSetLayout dsl; VkDescriptorPool dp;
VkPipelineLayout pl; VkPipeline pipe; VkCommandPool cp; VkCommandBuffer cb;

int main(int argc, char **argv)
{
    if (argc < 3) DIE("usage: %s <workgroups> <iters>", argv[0]);
    uint32_t wg = (uint32_t)strtoul(argv[1], NULL, 0);
    int iters = atoi(argv[2]);
    if (wg < 1 || wg > 65535 || iters < 1) DIE("bad arguments: wg=%u (1..65535) iters=%d", wg, iters);

    CK(vkCreateInstance(&(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_MAKE_VERSION(1,2,0) } }, NULL, &inst));
    uint32_t nd = 0; CK(vkEnumeratePhysicalDevices(inst, &nd, NULL));
    if (!nd) DIE("no physical devices");
    VkPhysicalDevice *pds = calloc(nd, sizeof(*pds));
    CK(vkEnumeratePhysicalDevices(inst, &nd, pds));
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s (api %u.%u.%u)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t nqf = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    int qi = -1;
    for (uint32_t i = 0; i < nqf; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = (int)i; break; }
    if (qi < 0) DIE("no compute queue");

    float prio = 1.0f;
    CK(vkCreateDevice(pd, &(VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = (uint32_t)qi, .queueCount = 1, .pQueuePriorities = &prio } },
        NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, (uint32_t)qi, 0, &q);

    /* output buffer: 64K uints, one per invocation index (masked) */
    VkBuffer buf; VkDeviceMemory mem;
    CK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536 * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == UINT32_MAX) DIE("no host-visible coherent memory for the output buffer");
    CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt }, NULL, &mem));
    CK(vkBindBufferMemory(dev, buf, mem, 0));

    CK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &(VkDescriptorSetLayoutBinding){
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } }, NULL, &dsl));
    CK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1,
        .pPoolSizes = &(VkDescriptorPoolSize){ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 } },
        NULL, &dp));
    VkDescriptorSet ds;
    CK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ds));
    VkWriteDescriptorSet wr = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &(VkDescriptorBufferInfo){ .buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4 };
    CK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr }, NULL, &pl));

    VkShaderModule sm;
    CK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(cstpf4_comp_spv), .pCode = (const uint32_t *)cstpf4_comp_spv }, NULL, &sm));
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = pl }, NULL, &pipe));
    printf("compute pipeline created\n");

    CK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)qi }, NULL, &cp));
    CK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));

    CK(vkResetCommandBuffer(cb, 0));
    CK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    for (int i = 0; i < iters; i++) {
        uint32_t n = (uint32_t)i;
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
        vkCmdDispatch(cb, wg, 1, 1);
    }
    CK(vkEndCommandBuffer(cb));

    VkFence fence; CK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));
    printf("dispatching %d x %u workgroups of 64 invocations (%d iters)...\n", iters, wg, iters);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CK(vkResetFences(dev, 1, &fence));
    CK(vkQueueSubmit(q, 1, &(VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    double invocations = (double)iters * wg * 64.0;
    printf("%.1f M invocation/s (%.3f ms total, %.4f ms/iter)\n",
           invocations / ms / 1e3, ms, ms / iters);

    vkDeviceWaitIdle(dev);
    return 0;
}
