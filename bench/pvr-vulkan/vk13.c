/* vk13.c - the three Vulkan 1.3 features the open driver did not have.
 *
 *   A. shaderZeroInitializeWorkgroupMemory - read workgroup memory nothing wrote.
 *   B. pipelineCreationCacheControl         - FAIL_ON_PIPELINE_COMPILE_REQUIRED.
 *   C. robustImageAccess                    - out-of-bounds image read.
 *
 * Each phase is a check that can fail, and each has a control that shows the
 * harness can tell the difference: A writes a sentinel in mode 1, C reads an
 * in-bounds texel alongside the out-of-bounds one.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./vk13
 *
 * Exit status 0 on PASS, 1 on FAIL, 2 on setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vulkan/vulkan.h>

#include "zero_shared_spv.h"
#include "robust_image_spv.h"

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

static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static VkCommandPool pool;

/* --- shared helpers ------------------------------------------------------- */

static uint32_t pick_memory(uint32_t type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    DIE("no memory type with flags 0x%x", want);
}

static VkBuffer make_buffer(VkDeviceSize size, VkDeviceMemory *mem, void **mapped)
{
    VkBuffer buf;

    VKCHECK(vkCreateBuffer(dev,
                           &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                  .size = size,
                                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
                           NULL,
                           &buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);

    VKCHECK(vkAllocateMemory(dev,
                             &(VkMemoryAllocateInfo){
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = pick_memory(req.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                             },
                             NULL,
                             mem));
    VKCHECK(vkBindBufferMemory(dev, buf, *mem, 0));

    if (mapped) {
        VKCHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
        memset(*mapped, 0xff, size);
    }

    return buf;
}

static void submit_and_wait(VkCommandBuffer cb)
{
    VkFence fence;

    VKCHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO },
                          NULL, &fence));
    VKCHECK(vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                           .commandBufferCount = 1,
                                           .pCommandBuffers = &cb },
                          fence));
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
    vkDestroyFence(dev, fence, NULL);
}

static VkCommandBuffer begin_cb(void)
{
    VkCommandBuffer cb;

    VKCHECK(vkAllocateCommandBuffers(dev,
                                     &(VkCommandBufferAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1,
                                     },
                                     &cb));
    VKCHECK(vkBeginCommandBuffer(cb,
                                 &(VkCommandBufferBeginInfo){
                                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                 }));
    return cb;
}

static VkPipeline make_compute_pipeline(const unsigned *spv, size_t spv_len,
                                        VkPipelineLayout layout, VkPipelineCreateFlags flags,
                                        const char *what)
{
    VkShaderModule sm;
    VkPipeline pipe;

    VKCHECK(vkCreateShaderModule(dev,
                                 &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                              .codeSize = spv_len,
                                                              .pCode = spv },
                                 NULL, &sm));

    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1,
                                          &(VkComputePipelineCreateInfo){
                                             .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                             .flags = flags,
                                             .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = sm,
                                                        .pName = "main" },
                                             .layout = layout,
                                          },
                                          NULL, &pipe);
    vkDestroyShaderModule(dev, sm, NULL);

    if (flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) {
        check(r == VK_PIPELINE_COMPILE_REQUIRED,
              "%s: FAIL_ON_PIPELINE_COMPILE_REQUIRED -> %s (%d)", what,
              r == VK_PIPELINE_COMPILE_REQUIRED ? "VK_PIPELINE_COMPILE_REQUIRED" : "unexpected",
              (int)r);
        return VK_NULL_HANDLE;
    }

    if (r != VK_SUCCESS)
        DIE("%s: vkCreateComputePipelines -> %d", what, (int)r);

    return pipe;
}

/* --- A: zero-initialised workgroup memory --------------------------------- */

static void phase_zero_shared(VkPhysicalDeviceVulkan13Features *f13)
{
    printf("A. shaderZeroInitializeWorkgroupMemory\n");
    check(f13->shaderZeroInitializeWorkgroupMemory,
          "device feature shaderZeroInitializeWorkgroupMemory = %d",
          f13->shaderZeroInitializeWorkgroupMemory);
    if (!f13->shaderZeroInitializeWorkgroupMemory)
        return;

    VkDeviceMemory mem;
    void *mapped;
    VkBuffer out = make_buffer(64 * sizeof(uint32_t), &mem, &mapped);

    VkDescriptorSetLayoutBinding b = { .binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(dev,
                                        &(VkDescriptorSetLayoutCreateInfo){
                                           .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1,
                                           .pBindings = &b },
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
                                         .size = sizeof(uint32_t) } },
                                   NULL, &layout));

    /* Phase B is folded in: the first creation asks not to block on a compile. */
    VkPipeline pipe = make_compute_pipeline(zero_shared_spv, sizeof(zero_shared_spv), layout,
                                            VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
                                            "zero_shared");
    VkPipeline pipe2 = make_compute_pipeline(zero_shared_spv, sizeof(zero_shared_spv), layout, 0,
                                             "zero_shared (retry without the flag)");
    check(pipe == VK_NULL_HANDLE && pipe2 != VK_NULL_HANDLE,
          "the retry without the flag produced a pipeline");

    VkDescriptorPool dp;
    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VKCHECK(vkCreateDescriptorPool(dev,
                                   &(VkDescriptorPoolCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &ps },
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
                                      .pBufferInfo = &(VkDescriptorBufferInfo){ .buffer = out,
                                                                                .range = VK_WHOLE_SIZE } },
                                   0, NULL);

    /* mode 0: read memory nothing wrote. mode 1: sentinel control. */
    for (uint32_t mode = 0; mode <= 1; mode++) {
        VkCommandBuffer cb = begin_cb();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe2);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
        vkCmdDispatch(cb, 1, 1, 1);
        VKCHECK(vkEndCommandBuffer(cb));
        submit_and_wait(cb);
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        VKCHECK(vkQueueWaitIdle(queue));

        uint32_t *v = mapped;
        uint32_t nonzero = 0;
        for (int i = 0; i < 64; i++)
            nonzero += v[i] != 0;

        if (mode == 0)
            check(nonzero == 0, "workgroup memory with a null initializer reads back as %u non-zero of 64 (first=0x%08x)",
                  nonzero, v[0]);
        else
            check(v[0] == 0xDEADBEEFu && nonzero == 64,
                  "control: sentinel written first reads back as 0x%08x in %u of 64 (harness can see values)",
                  v[0], nonzero);
    }

    vkDestroyPipeline(dev, pipe2, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyBuffer(dev, out, NULL);
    vkFreeMemory(dev, mem, NULL);
}

/* --- C: robust image access ----------------------------------------------- */

static void phase_robust_image(VkPhysicalDeviceVulkan13Features *f13)
{
    printf("C. robustImageAccess\n");
    check(f13->robustImageAccess, "device feature robustImageAccess = %d", f13->robustImageAccess);
    if (!f13->robustImageAccess)
        return;

    /* A 4x4 storage image, cleared to a known colour, and a shader that reads
     * (0,0) and (4096,4096). */
    VkImage img;
    VKCHECK(vkCreateImage(dev,
                          &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                                .imageType = VK_IMAGE_TYPE_2D,
                                                .format = VK_FORMAT_R8G8B8A8_UINT,
                                                .extent = { 4, 4, 1 },
                                                .mipLevels = 1,
                                                .arrayLayers = 1,
                                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED },
                          NULL, &img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkDeviceMemory imem;
    VKCHECK(vkAllocateMemory(dev,
                             &(VkMemoryAllocateInfo){
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = pick_memory(req.memoryTypeBits, 0) },
                             NULL, &imem));
    VKCHECK(vkBindImageMemory(dev, img, imem, 0));

    VkImageView iv;
    VKCHECK(vkCreateImageView(dev,
                              &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                        .image = img,
                                                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                        .format = VK_FORMAT_R8G8B8A8_UINT,
                                                        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                              .levelCount = 1,
                                                                              .layerCount = 1 } },
                              NULL, &iv));

    VkDeviceMemory omem;
    void *mapped;
    VkBuffer out = make_buffer(2 * sizeof(uint32_t) * 4, &omem, &mapped);

    VkCommandBuffer cb = begin_cb();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL,
                         1, &(VkImageMemoryBarrier){
                               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .levelCount = 1,
                                                     .layerCount = 1 },
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT });
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &(VkClearColorValue){ .uint32 = { 64, 128, 191, 255 } }, 1,
                         &(VkImageSubresourceRange){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .levelCount = 1,
                                                     .layerCount = 1 });
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL,
                         1, &(VkImageMemoryBarrier){
                               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .levelCount = 1,
                                                     .layerCount = 1 },
                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT });

    VkDescriptorSetLayoutBinding binds[2] = {
        { .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
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
                                           .pBindings = binds },
                                        NULL, &dsl));
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(dev,
                                   &(VkPipelineLayoutCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &dsl },
                                   NULL, &layout));
    VkPipeline pipe = make_compute_pipeline(robust_image_spv, sizeof(robust_image_spv), layout, 0,
                                            "robust_image");

    VkDescriptorPool dp;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VKCHECK(vkCreateDescriptorPool(dev,
                                   &(VkDescriptorPoolCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 2,
                                      .pPoolSizes = ps },
                                   NULL, &dp));
    VkDescriptorSet ds;
    VKCHECK(vkAllocateDescriptorSets(dev,
                                     &(VkDescriptorSetAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &dsl },
                                     &ds));
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &(VkDescriptorImageInfo){ .imageView = iv,
                                                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL } },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ds,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &(VkDescriptorBufferInfo){ .buffer = out, .range = VK_WHOLE_SIZE } },
    };
    vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VKCHECK(vkEndCommandBuffer(cb));
    submit_and_wait(cb);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    VKCHECK(vkQueueWaitIdle(queue));

    uint32_t *v = mapped;
    uint32_t inb[4] = { v[0], v[1], v[2], v[3] };
    uint32_t oob[4] = { v[4], v[5], v[6], v[7] };

    printf("  in-bounds  read = %u,%u,%u,%u\n", inb[0], inb[1], inb[2], inb[3]);
    printf("  out-of-bounds  = %u,%u,%u,%u\n", oob[0], oob[1], oob[2], oob[3]);

    check(inb[0] == 64 && inb[1] == 128 && inb[2] == 191 && inb[3] == 255,
          "in-bounds read is the cleared colour (64,128,191,255)");
    check(oob[0] == 0 && oob[1] == 0 && oob[2] == 0 && oob[3] <= 1,
          "out-of-bounds read is zero (alpha 0 or 1), not undefined data");

    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyBuffer(dev, out, NULL);
    vkFreeMemory(dev, omem, NULL);
    vkDestroyImageView(dev, iv, NULL);
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, imem, NULL);
}

int main(void)
{
    VkInstance instance;
    VKCHECK(vkCreateInstance(&(VkInstanceCreateInfo){
                                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &(VkApplicationInfo){
                                   .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3 } },
                             NULL, &instance));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (!ndev)
        DIE("no device");
    VkPhysicalDevice devs[4];
    if (ndev > 4)
        ndev = 4;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));
    phys = devs[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    check(VK_VERSION_MAJOR(props.apiVersion) == 1 && VK_VERSION_MINOR(props.apiVersion) >= 3,
          "device reports Vulkan 1.3 or later (%u.%u)", VK_VERSION_MAJOR(props.apiVersion),
          VK_VERSION_MINOR(props.apiVersion));

    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                     .pNext = &f13 };
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    printf("B. pipelineCreationCacheControl\n");
    check(f13.pipelineCreationCacheControl, "device feature pipelineCreationCacheControl = %d",
          f13.pipelineCreationCacheControl);

    uint32_t qf = 0;
    float prio = 1.0f;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = &f13,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qf,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio } };
    VkResult dr = vkCreateDevice(phys, &dci, NULL, &dev);
    check(dr == VK_SUCCESS, "vkCreateDevice with the 1.3 features enabled -> %d", (int)dr);
    if (dr != VK_SUCCESS)
        goto report;

    vkGetDeviceQueue(dev, qf, 0, &queue);
    VKCHECK(vkCreateCommandPool(dev,
                                &(VkCommandPoolCreateInfo){
                                   .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = qf },
                                NULL, &pool));

    phase_zero_shared(&f13);
    phase_robust_image(&f13);

report:
    printf("VERDICT: %s (%d ok, %d failed)\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
