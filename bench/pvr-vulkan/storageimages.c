/* storageimages - probe maxPerStageDescriptorStorageImages.
 * The driver reports 4, which is exactly the Vulkan minimum and has no backing constant
 * in the driver, so it may be a floor rather than the hardware's limit.
 * usage: ./storageimages <spv>   (shader built with -DNIMG=<n>)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifndef NIMG
#define NIMG 4
#endif
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
    printf("reported maxPerStageDescriptorStorageImages = %u  maxDescriptorSetStorageImages = %u\n",
           pr.limits.maxPerStageDescriptorStorageImages, pr.limits.maxDescriptorSetStorageImages);
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CK(vkCreateDevice(pd[0],&dci,NULL,&dev),"device");
    VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0],&mp);
    uint32_t hv=0; for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){hv=i;break;}
    VkImage img[NIMG]; VkDeviceMemory im[NIMG]; VkImageView iv[NIMG];
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    for(int i=0;i<NIMG;i++){
        CK(vkCreateImage(dev,&ic,NULL,&img[i]),"image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img[i],&mr);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hv};
        CK(vkAllocateMemory(dev,&ma,NULL,&im[i]),"mem");
        CK(vkBindImageMemory(dev,img[i],im[i],0),"bind");
        VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CK(vkCreateImageView(dev,&vc,NULL,&iv[i]),"view");
    }
    VkBufferCreateInfo ob={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=NIMG*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer out; CK(vkCreateBuffer(dev,&ob,NULL,&out),"outbuf");
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(dev,out,&omr);
    VkMemoryAllocateInfo oma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=omr.size,.memoryTypeIndex=hv};
    VkDeviceMemory om; CK(vkAllocateMemory(dev,&oma,NULL,&om),"outmem");
    CK(vkBindBufferMemory(dev,out,om,0),"bindout");
    void*op; CK(vkMapMemory(dev,om,0,omr.size,0,&op),"map"); memset(op,0,omr.size);
    VkDescriptorSetLayoutBinding b[2]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=NIMG,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"dsl");
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,NIMG},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev,&dp,NULL,&pool),"pool");
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&da,&ds),"set");
    VkDescriptorImageInfo dii[NIMG];
    for(int i=0;i<NIMG;i++){dii[i].sampler=VK_NULL_HANDLE;dii[i].imageView=iv[i];dii[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;}
    VkDescriptorBufferInfo dbi={.buffer=out,.offset=0,.range=VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=NIMG,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=dii},
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
    VkPipeline pipe; VkResult r=vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cp,NULL,&pipe);
    printf("vkCreateComputePipelines (%d storage images) -> %d%s\n",NIMG,(int)r,r==VK_SUCCESS?"":"  <-- REJECTED");
    if(r!=VK_SUCCESS){printf("VERDICT: driver rejects %d storage images per stage\n",NIMG);return 1;}
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp2; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp2),"cmdpool");
    VkCommandBufferAllocateInfo cba={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp2,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cba,&cb),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=0,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    for(int i=0;i<NIMG;i++){bar.image=img[i];vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&bar);}
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pll,0,1,&ds,0,NULL);
    vkCmdDispatch(cb,1,1,1);
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");
    uint32_t*o=op; int good=0,zero=0,bad=0;
    for(int i=0;i<NIMG;i++){ if(o[i]==(uint32_t)i)good++; else if(!o[i])zero++; else bad++; }
    printf("%d storage images: %d correct, %d untouched(0), %d wrong\n",NIMG,good,zero,bad);
    printf("VERDICT: %s (%d storage images per stage)\n",
           (good==NIMG)?"storage images work correctly":"storage images do NOT work",NIMG);
    return good==NIMG?0:1;
}
