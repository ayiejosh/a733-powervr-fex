/* vkaudit - dump a Vulkan driver's capabilities in a stable, diffable form.
 *
 * The point is comparison: run it once per ICD and diff the output to see what one
 * driver offers that the other does not. Feature booleans print as name=0/1 so the
 * diff is readable, and extensions print one per line.
 *
 *   VK_ICD_FILENAMES=<icd.json> ./vkaudit > out.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

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

static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void)
{
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst;
    VKCHECK(vkCreateInstance(&ici, NULL, &inst));

    printf("== instance extensions ==\n");
    {
        uint32_t n = 0;
        VKCHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL));
        VkExtensionProperties *ext = calloc(n ? n : 1, sizeof(*ext));
        VKCHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, ext));
        const char **names = calloc(n ? n : 1, sizeof(*names));
        for (uint32_t i = 0; i < n; i++)
            names[i] = ext[i].extensionName;
        qsort(names, n, sizeof(*names), cmpstr);
        for (uint32_t i = 0; i < n; i++)
            printf("  %s\n", names[i]);
        free(names);
        free(ext);
    }

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    if (!ndev)
        DIE("no Vulkan device");
    VkPhysicalDevice *pdevs = calloc(ndev, sizeof(*pdevs));
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, pdevs));

    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDevice pd = pdevs[d];
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        printf("\n== device %u ==\n", d);
        printf("  deviceName    %s\n", p.deviceName);
        printf("  apiVersion    %u.%u.%u\n", VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
        printf("  driverVersion 0x%08x\n", p.driverVersion);
        printf("  deviceType    %d\n", (int)p.deviceType);

        printf("\n-- limits (selected) --\n");
        printf("  maxImageDimension2D            %u\n", p.limits.maxImageDimension2D);
        printf("  maxImageDimensionCube          %u\n", p.limits.maxImageDimensionCube);
        printf("  maxFramebufferWidth/Height     %u/%u\n", p.limits.maxFramebufferWidth,
               p.limits.maxFramebufferHeight);
        printf("  maxColorAttachments            %u\n", p.limits.maxColorAttachments);
        printf("  maxComputeWorkGroupCount       %u %u %u\n", p.limits.maxComputeWorkGroupCount[0],
               p.limits.maxComputeWorkGroupCount[1], p.limits.maxComputeWorkGroupCount[2]);
        printf("  maxComputeWorkGroupSize        %u %u %u\n", p.limits.maxComputeWorkGroupSize[0],
               p.limits.maxComputeWorkGroupSize[1], p.limits.maxComputeWorkGroupSize[2]);
        printf("  maxComputeWorkGroupInvocations %u\n", p.limits.maxComputeWorkGroupInvocations);
        printf("  maxComputeSharedMemorySize     %u\n", p.limits.maxComputeSharedMemorySize);
        printf("  maxDescriptorSetSamplers       %u\n", p.limits.maxDescriptorSetSamplers);
        printf("  maxPerStageDescriptorSamplers  %u\n", p.limits.maxPerStageDescriptorSamplers);
        printf("  maxVertexInputAttributes       %u\n", p.limits.maxVertexInputAttributes);
        printf("  maxViewportDimensions          %u %u\n", p.limits.maxViewportDimensions[0],
               p.limits.maxViewportDimensions[1]);
        printf("  maxBoundDescriptorSets         %u\n", p.limits.maxBoundDescriptorSets);
        printf("  timestampPeriod                %.3f\n", p.limits.timestampPeriod);
        printf("  framebufferColorSampleCounts   0x%x\n", p.limits.framebufferColorSampleCounts);
        printf("  framebufferDepthSampleCounts   0x%x\n", p.limits.framebufferDepthSampleCounts);
        printf("  sampledImageColorSampleCounts  0x%x\n", p.limits.sampledImageColorSampleCounts);

        printf("\n-- device extensions --\n");
        uint32_t n = 0;
        VKCHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL));
        VkExtensionProperties *ext = calloc(n ? n : 1, sizeof(*ext));
        VKCHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ext));
        const char **names = calloc(n ? n : 1, sizeof(*names));
        for (uint32_t i = 0; i < n; i++)
            names[i] = ext[i].extensionName;
        qsort(names, n, sizeof(*names), cmpstr);
        for (uint32_t i = 0; i < n; i++)
            printf("  %s\n", names[i]);
        free(names);
        free(ext);

        printf("\n-- features --\n");
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceVulkan11Features f11 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pd, &f2);

#define F(name) printf("  core." #name "=%d\n", f2.features.name)
        F(geometryShader);
        F(tessellationShader);
        F(depthClamp);
        F(depthBiasClamp);
        F(fillModeNonSolid);
        F(wideLines);
        F(largePoints);
        F(independentBlend);
        F(sampleRateShading);
        F(multiViewport);
        F(imageCubeArray);
        F(shaderStorageImageExtendedFormats);
        F(occlusionQueryPrecise);
        F(pipelineStatisticsQuery);
        F(shaderStorageImageWriteWithoutFormat);
        F(shaderStorageImageReadWithoutFormat);
        F(fragmentStoresAndAtomics);
        F(vertexPipelineStoresAndAtomics);
#undef F
#define F11(name) printf("  vk11." #name "=%d\n", f11.name)
        F11(shaderDrawParameters);
        F11(storageBuffer16BitAccess);
        F11(uniformAndStorageBuffer16BitAccess);
        F11(storagePushConstant16);
        F11(storageInputOutput16);
        F11(multiview);
        F11(variablePointersStorageBuffer);
        F11(variablePointers);
        F11(protectedMemory);
        F11(samplerYcbcrConversion);
#undef F11
#define F12(name) printf("  vk12." #name "=%d\n", f12.name)
        F12(samplerMirrorClampToEdge);
        F12(drawIndirectCount);
        F12(storageBuffer8BitAccess);
        F12(uniformAndStorageBuffer8BitAccess);
        F12(storagePushConstant8);
        F12(shaderBufferInt64Atomics);
        F12(shaderSharedInt64Atomics);
        F12(shaderFloat16);
        F12(shaderInt8);
        F12(descriptorIndexing);
        F12(shaderInputAttachmentArrayDynamicIndexing);
        F12(shaderUniformTexelBufferArrayDynamicIndexing);
        F12(shaderStorageTexelBufferArrayDynamicIndexing);
        F12(shaderUniformBufferArrayNonUniformIndexing);
        F12(shaderSampledImageArrayNonUniformIndexing);
        F12(shaderStorageBufferArrayNonUniformIndexing);
        F12(shaderStorageImageArrayNonUniformIndexing);
        F12(shaderInputAttachmentArrayNonUniformIndexing);
        F12(descriptorBindingUniformBufferUpdateAfterBind);
        F12(descriptorBindingSampledImageUpdateAfterBind);
        F12(descriptorBindingStorageImageUpdateAfterBind);
        F12(descriptorBindingStorageBufferUpdateAfterBind);
        F12(descriptorBindingUpdateUnusedWhilePending);
        F12(descriptorBindingPartiallyBound);
        F12(descriptorBindingVariableDescriptorCount);
        F12(runtimeDescriptorArray);
        F12(samplerFilterMinmax);
        F12(scalarBlockLayout);
        F12(imagelessFramebuffer);
        F12(uniformBufferStandardLayout);
        F12(shaderSubgroupExtendedTypes);
        F12(separateDepthStencilLayouts);
        F12(hostQueryReset);
        F12(timelineSemaphore);
        F12(bufferDeviceAddress);
        F12(bufferDeviceAddressCaptureReplay);
        F12(vulkanMemoryModel);
        F12(vulkanMemoryModelDeviceScope);
        F12(subgroupBroadcastDynamicId);
#undef F12
#define F13(name) printf("  vk13." #name "=%d\n", f13.name)
        F13(robustImageAccess);
        F13(inlineUniformBlock);
        F13(descriptorBindingInlineUniformBlockUpdateAfterBind);
        F13(pipelineCreationCacheControl);
        F13(privateData);
        F13(shaderDemoteToHelperInvocation);
        F13(shaderTerminateInvocation);
        F13(subgroupSizeControl);
        F13(computeFullSubgroups);
        F13(synchronization2);
        F13(textureCompressionASTC_HDR);
        F13(shaderZeroInitializeWorkgroupMemory);
        F13(dynamicRendering);
        F13(shaderIntegerDotProduct);
        F13(maintenance4);
#undef F13

        printf("\n-- subgroup --\n");
        VkPhysicalDeviceSubgroupProperties sg = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &sg };
        vkGetPhysicalDeviceProperties2(pd, &p2);
        printf("  subgroupSize=%u supportedStages=0x%x supportedOperations=0x%x quadOperations=0x%x\n",
               sg.subgroupSize, sg.supportedStages, sg.supportedOperations,
               sg.quadOperationsInAllStages);
    }

    vkDestroyInstance(inst, NULL);
    return 0;
}
