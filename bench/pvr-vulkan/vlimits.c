/* vlimits - dump the limits Vulkan's invariants depend on, so they can be checked against the
 * spec rather than against the driver's source. vkaudit only prints a subset. */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
int main(void){
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; if(vkCreateInstance(&ici,NULL,&inst)!=VK_SUCCESS){puts("instance failed");return 1;}
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL);
    VkPhysicalDevice*pd=malloc(n*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&n,pd);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[0],&p);
    const VkPhysicalDeviceLimits*l=&p.limits;
    printf("device %s  api %u.%u.%u\n",p.deviceName,VK_VERSION_MAJOR(p.apiVersion),
           VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion));
    printf("maxImageDimension1D %u  2D %u  3D %u  Cube %u\n",l->maxImageDimension1D,
           l->maxImageDimension2D,l->maxImageDimension3D,l->maxImageDimensionCube);
    printf("maxImageArrayLayers %u\n",l->maxImageArrayLayers);
    printf("maxFramebufferWidth %u  Height %u  Layers %u\n",l->maxFramebufferWidth,
           l->maxFramebufferHeight,l->maxFramebufferLayers);
    printf("maxViewportDimensions %u %u  maxViewports %u\n",l->maxViewportDimensions[0],
           l->maxViewportDimensions[1],l->maxViewports);
    printf("maxColorAttachments %u  maxClipDistances %u  maxCullDistances %u  maxCombinedClipAndCullDistances %u\n",
           l->maxColorAttachments,l->maxClipDistances,l->maxCullDistances,l->maxCombinedClipAndCullDistances);
    printf("maxFragmentInputComponents %u  maxVertexOutputComponents %u\n",
           l->maxFragmentInputComponents,l->maxVertexOutputComponents);
    printf("maxVertexInputAttributes %u  Bindings %u  BindingStride %u  AttributeOffset %u\n",
           l->maxVertexInputAttributes,l->maxVertexInputBindings,l->maxVertexInputBindingStride,
           l->maxVertexInputAttributeOffset);
    printf("maxPushConstantsSize %u\n",l->maxPushConstantsSize);
    printf("maxMemoryAllocationCount %u  maxSamplerAllocationCount %u\n",
           l->maxMemoryAllocationCount,l->maxSamplerAllocationCount);
    printf("bufferImageGranularity %llu  sparseAddressSpaceSize %llu\n",
           (unsigned long long)l->bufferImageGranularity,(unsigned long long)l->sparseAddressSpaceSize);
    printf("maxSampleMaskWords %u  maxDrawIndirectCount %u\n",l->maxSampleMaskWords,l->maxDrawIndirectCount);
    printf("maxComputeWorkGroupCount %u %u %u  Size %u %u %u  Invocations %u\n",
           l->maxComputeWorkGroupCount[0],l->maxComputeWorkGroupCount[1],l->maxComputeWorkGroupCount[2],
           l->maxComputeWorkGroupSize[0],l->maxComputeWorkGroupSize[1],l->maxComputeWorkGroupSize[2],
           l->maxComputeWorkGroupInvocations);
    printf("maxComputeSharedMemorySize %u\n",l->maxComputeSharedMemorySize);
    printf("maxPerStageResources %u  maxBoundDescriptorSets %u\n",l->maxPerStageResources,l->maxBoundDescriptorSets);
    printf("perStage/stage: samplers %u/%u ub %u/%u sb %u/%u si %u/%u ii %u/%u inatt %u/%u\n",
           l->maxPerStageDescriptorSamplers,l->maxDescriptorSetSamplers,
           l->maxPerStageDescriptorUniformBuffers,l->maxDescriptorSetUniformBuffers,
           l->maxPerStageDescriptorStorageBuffers,l->maxDescriptorSetStorageBuffers,
           l->maxPerStageDescriptorSampledImages,l->maxDescriptorSetSampledImages,
           l->maxPerStageDescriptorStorageImages,l->maxDescriptorSetStorageImages,
           l->maxPerStageDescriptorInputAttachments,l->maxDescriptorSetInputAttachments);
    printf("maxFragmentCombinedOutputResources %u\n",l->maxFragmentCombinedOutputResources);
    printf("maxInterpolationOffset %g  subPixelInterpolationOffsetBits %u\n",
           (double)l->maxInterpolationOffset,l->subPixelInterpolationOffsetBits);
    printf("standardSampleLocations %d  maxSamplerLodBias %g\n",l->standardSampleLocations,(double)l->maxSamplerLodBias);
    return 0;
}
