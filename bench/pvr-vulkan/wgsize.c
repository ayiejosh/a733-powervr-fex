/* wgsize - probe whether a compute dispatch with more than the reported
 * maxComputeWorkGroupInvocations actually runs correctly.
 *
 * The driver reports 128 (the Vulkan minimum) while the vendor reports 512 on the same
 * silicon, so the question is whether 128 is a hardware floor or just an under-reported
 * limit. Each invocation writes id^0xa5a5a5a5, so the readback proves both how many
 * invocations ran and that each wrote its own slot.
 *
 * usage: ./wgsize <spv>  (shader built with -DWGSIZE=<n>)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x, msg) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FAIL %s -> %d\n", msg, (int)_r); exit(1); } } while (0)

static uint32_t *read_spv(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); exit(1); }
    fclose(f); *n = sz; return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <spv>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);

    size_t spv_n;
    uint32_t *spv = read_spv(argv[1], &spv_n);

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    uint32_t np = 0;
    vkEnumeratePhysicalDevices(inst, &np, NULL);
    if (!np) { fprintf(stderr, "no devices\n"); return 1; }
    VkPhysicalDevice *pds = malloc(np * sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &np, pds);
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));
    printf("reported maxComputeWorkGroupInvocations = %u\n",
           props.limits.maxComputeWorkGroupInvocations);
    printf("reported maxComputeWorkGroupSize = %u %u %u\n",
           props.limits.maxComputeWorkGroupSize[0],
           props.limits.maxComputeWorkGroupSize[1],
           props.limits.maxComputeWorkGroupSize[2]);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* Output buffer: N uints, host visible. */
    const uint32_t N = 512;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = N * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;
    CK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
            { mt = i; break; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = mt };
    VkDeviceMemory mem;
    CK(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory");
    CK(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");
    void *ptr;
    CK(vkMapMemory(dev, mem, 0, mr.size, 0, &ptr), "vkMapMemory");
    memset(ptr, 0, mr.size);

    VkDescriptorSetLayoutBinding dslb = { .binding = 0,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .descriptorCount = 1,
                                          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 1, .pBindings = &dslb };
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = dp, .descriptorSetCount = 1,
                                         .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    CK(vkAllocateDescriptorSets(dev, &dsai, &ds), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl), "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = spv_n, .pCode = spv };
    VkShaderModule sm;
    CK(vkCreateShaderModule(dev, &smci, NULL, &sm), "vkCreateShaderModule");

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = pl };
    VkPipeline pipe;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    printf("vkCreateComputePipelines(local_size_x=512) -> %d%s\n", (int)pr,
           pr == VK_SUCCESS ? "" : "  <-- REJECTED");
    if (pr != VK_SUCCESS) { printf("VERDICT: driver rejects a 512-invocation workgroup\n"); return 1; }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = 0 };
    VkCommandPool cp;
    CK(vkCreateCommandPool(dev, &cpi, NULL, &cp), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CK(vkBeginCommandBuffer(cb, &cbbi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);   /* ONE workgroup of 512 invocations */
    CK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    CK(vkQueueWaitIdle(q), "vkQueueWaitIdle");

    uint32_t *out = ptr;
    uint32_t good = 0, zero = 0, bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (out[i] == (i ^ 0xa5a5a5a5u)) good++;
        else if (out[i] == 0) zero++;
        else bad++;
    }
    printf("512 invocations: %u correct, %u untouched(0), %u wrong\n", good, zero, bad);
    static const uint32_t probe[] = {0, 1, 63, 64, 127, 128, 255, 256, 511};
    for (unsigned i = 0; i < sizeof(probe)/sizeof(probe[0]); i++) {
        uint32_t k = probe[i];
        printf("  slot[%3u] %s  got 0x%08x want 0x%08x\n", k,
               out[k] == (k ^ 0xa5a5a5a5u) ? "ok  " : "BAD ", out[k], k ^ 0xa5a5a5a5u);
    }
    printf("VERDICT: %s\n", (good == N) ? "512-invocation workgroup works correctly"
                                        : "512-invocation workgroup does NOT work");
    return (good == N) ? 0 : 1;
}
