/* linfilter - can the driver actually linearly filter a 32-bit float texture?
 *
 * Vulkan MANDATES VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT for
 * VK_FORMAT_R32G32B32A32_SFLOAT, but pvr_formats.c only advertises it when
 * first_component_size < 32, so every 32-bit float format lacks it.
 *
 * A 2x1 R32_SFLOAT image holding 0.0 and 1.0 is sampled at u = 0.5 with a LINEAR
 * sampler. A linear filter must return ~0.5 (the average); a NEAREST filter returns 0 or 1.
 * Both filters are run, so the test also proves the sampling path itself works.
 *
 * usage: ./linfilter <spv>   (shader built with -DFMT=... to select the format)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x,m) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",m,(int)r);exit(1);}}while(0)
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <spv>\n",argv[0]);return 2;}
    setvbuf(stdout,NULL,_IONBF,0);
    FILE*f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint32_t*spv=malloc(sz); if(fread(spv,1,sz,f)!=(size_t)sz)return 1; fclose(f);
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst),"instance");
    uint32_t np=0; vkEnumeratePhysicalDevices(inst,&np,NULL);
    VkPhysicalDevice*pd=malloc(np*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&np,pd);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[0],&pr);
    printf("device: %s\n",pr.deviceName);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd[0],VK_FORMAT_R32_SFLOAT,&fp);
    printf("R32_SFLOAT optimalTilingFeatures = 0x%08x  FILTER_LINEAR advertised = %d\n",
           fp.optimalTilingFeatures,
           !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CK(vkCreateDevice(pd[0],&dci,NULL,&dev),"device");
    VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0],&mp);
    uint32_t hv=0; for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){hv=i;break;}
    /* 2x1 R32_SFLOAT image, filled via a staging buffer */
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R32_SFLOAT,.extent={2,1,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; CK(vkCreateImage(dev,&ic,NULL,&img),"image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hv};
    VkDeviceMemory im; CK(vkAllocateMemory(dev,&ma,NULL,&im),"imagemem"); CK(vkBindImageMemory(dev,img,im,0),"bind");
    VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView iv; CK(vkCreateImageView(dev,&vc,NULL,&iv),"view");
    float px[2]={0.0f,1.0f};
    VkBufferCreateInfo sb={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizeof(px),.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer stage; CK(vkCreateBuffer(dev,&sb,NULL,&stage),"stage");
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(dev,stage,&smr);
    VkMemoryAllocateInfo sma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=smr.size,.memoryTypeIndex=hv};
    VkDeviceMemory smem; CK(vkAllocateMemory(dev,&sma,NULL,&smem),"stagemem"); CK(vkBindBufferMemory(dev,stage,smem,0),"bindstage");
    void*p; CK(vkMapMemory(dev,smem,0,smr.size,0,&p),"map"); memcpy(p,px,sizeof(px)); vkUnmapMemory(dev,smem);
    /* output */
    VkBufferCreateInfo ob={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer out; CK(vkCreateBuffer(dev,&ob,NULL,&out),"outbuf");
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(dev,out,&omr);
    VkMemoryAllocateInfo oma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=omr.size,.memoryTypeIndex=hv};
    VkDeviceMemory om; CK(vkAllocateMemory(dev,&oma,NULL,&om),"outmem"); CK(vkBindBufferMemory(dev,out,om,0),"bindout");
    void*op; CK(vkMapMemory(dev,om,0,omr.size,0,&op),"mapout"); memset(op,0,omr.size);
    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,
        .minFilter=VK_FILTER_LINEAR,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler smp; CK(vkCreateSampler(dev,&sci,NULL,&smp),"sampler");
    VkDescriptorSetLayoutBinding b[2]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"dsl");
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev,&dp,NULL,&pool),"pool");
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&da,&ds),"set");
    VkDescriptorImageInfo dii={.sampler=smp,.imageView=iv,.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo dbi={.buffer=out,.offset=0,.range=VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&dii},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=1,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi}};
    vkUpdateDescriptorSets(dev,2,w,0,NULL);
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    VkPipelineLayout pll; CK(vkCreatePipelineLayout(dev,&pl,NULL,&pll),"pl");
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sz,.pCode=spv};
    VkShaderModule mod; CK(vkCreateShaderModule(dev,&sm,NULL,&mod),"mod");
    VkComputePipelineCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=mod,.pName="main"},
        .layout=pll};
    VkPipeline pipe; CK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cp,NULL,&pipe),"pipeline");
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp2; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp2),"cmdpool");
    VkCommandBufferAllocateInfo cba={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp2,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cba,&cb),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.image=img};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&bar);
    VkBufferImageCopy bic={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={0,0,0},.imageExtent={2,1,1}};
    vkCmdCopyBufferToImage(cb,stage,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bic);
    bar.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&bar);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pll,0,1,&ds,0,NULL);
    vkCmdDispatch(cb,1,1,1);
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");
    float*o=op;
    printf("linear filter at u=0.5 of {0.0, 1.0}: %.4f  (want ~0.5)\n", o[0]);
    printf("VERDICT: %s\n", (o[0] > 0.4f && o[0] < 0.6f)
           ? "the hardware DOES linearly filter R32_SFLOAT - the feature bit is missing"
           : (o[0] < 0.1f || o[0] > 0.9f)
             ? "no interpolation - the hardware does NOT linearly filter R32_SFLOAT"
             : "inconclusive");
    return 0;
}
