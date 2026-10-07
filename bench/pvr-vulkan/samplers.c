/* samplers - probe maxPerStageDescriptorSamplers.
 *
 * The driver reports 16 (the Vulkan minimum); the vendor reports 32 on the same silicon.
 * This binds a 32-element sampler2D array, where texture i is a 1x1 image holding the
 * value i, and the shader writes each sampled value to slot i. The readback therefore
 * proves both how many samplers are usable and that each index reads the RIGHT texture -
 * an array that silently aliased or ran out would show wrong values, not just zeros.
 *
 * usage: ./samplers <spv>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x,m) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",m,(int)r);exit(1);}}while(0)
#ifndef NS
#define NS 32
#endif
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <spv>\n",argv[0]);return 2;}
    setvbuf(stdout,NULL,_IONBF,0);
    FILE*f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint32_t*spv=malloc(sz); if(fread(spv,1,sz,f)!=(size_t)sz){return 1;} fclose(f);

    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst),"instance");
    uint32_t np=0; vkEnumeratePhysicalDevices(inst,&np,NULL);
    VkPhysicalDevice*pd=malloc(np*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&np,pd);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[0],&pr);
    printf("device: %s\n",pr.deviceName);
    printf("reported maxPerStageDescriptorSamplers = %u  maxPerStageDescriptorSampledImages = %u\n",
           pr.limits.maxPerStageDescriptorSamplers, pr.limits.maxPerStageDescriptorSampledImages);
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CK(vkCreateDevice(pd[0],&dci,NULL,&dev),"device");
    VkQueue q; vkGetDeviceQueue(dev,0,0,&q);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0],&mp);
    uint32_t hostvis=0; for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){hostvis=i;break;}

    /* one 1x1 image per sampler, value = index */
    VkImage img[NS]; VkDeviceMemory imem[NS];
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    for(int i=0;i<NS;i++){
        CK(vkCreateImage(dev,&ic,NULL,&img[i]),"image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img[i],&mr);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hostvis};
        CK(vkAllocateMemory(dev,&ma,NULL,&imem[i]),"imagemem");
        CK(vkBindImageMemory(dev,img[i],imem[i],0),"bindimage");
    }
    /* staging buffer holding 32 RGBA pixels, pixel i = (i,i,i,255) */
    unsigned char px[NS*4]; for(int i=0;i<NS;i++){px[i*4]=i;px[i*4+1]=i;px[i*4+2]=i;px[i*4+3]=255;}
    VkBufferCreateInfo sb={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizeof(px),.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer stage; CK(vkCreateBuffer(dev,&sb,NULL,&stage),"stage");
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(dev,stage,&smr);
    VkMemoryAllocateInfo sma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=smr.size,.memoryTypeIndex=hostvis};
    VkDeviceMemory smem; CK(vkAllocateMemory(dev,&sma,NULL,&smem),"stagemem");
    CK(vkBindBufferMemory(dev,stage,smem,0),"bindstage");
    void*p; CK(vkMapMemory(dev,smem,0,smr.size,0,&p),"map"); memcpy(p,px,sizeof(px)); vkUnmapMemory(dev,smem);

    /* output buffer */
    VkBufferCreateInfo ob={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=NS*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer out; CK(vkCreateBuffer(dev,&ob,NULL,&out),"outbuf");
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(dev,out,&omr);
    VkMemoryAllocateInfo oma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=omr.size,.memoryTypeIndex=hostvis};
    VkDeviceMemory omem; CK(vkAllocateMemory(dev,&oma,NULL,&omem),"outmem");
    CK(vkBindBufferMemory(dev,out,omem,0),"bindout");
    void*op; CK(vkMapMemory(dev,omem,0,omr.size,0,&op),"mapout"); memset(op,0,omr.size);

    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,
        .minFilter=VK_FILTER_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler smp; CK(vkCreateSampler(dev,&sci,NULL,&smp),"sampler");

    VkDescriptorSetLayoutBinding b[2]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=NS,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"dsl");
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,NS},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev,&dp,NULL,&pool),"pool");
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&da,&ds),"allocset");
    VkDescriptorImageInfo dii[NS]; for(int i=0;i<NS;i++){dii[i].sampler=smp;dii[i].imageView=VK_NULL_HANDLE;dii[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
    VkImageView iv[NS];
    for(int i=0;i<NS;i++){
        VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CK(vkCreateImageView(dev,&vc,NULL,&iv[i]),"imageview"); dii[i].imageView=iv[i];
    }
    VkDescriptorBufferInfo dbi={.buffer=out,.offset=0,.range=VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=NS,
         .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=dii},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=1,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi}};
    vkUpdateDescriptorSets(dev,2,w,0,NULL);
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    VkPipelineLayout pll; CK(vkCreatePipelineLayout(dev,&pl,NULL,&pll),"pipelayout");
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sz,.pCode=spv};
    VkShaderModule mod; CK(vkCreateShaderModule(dev,&sm,NULL,&mod),"shadermodule");
    VkComputePipelineCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=mod,.pName="main"},
        .layout=pll};
    VkPipeline pipe; VkResult r=vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cp,NULL,&pipe);
    printf("vkCreateComputePipelines (%d samplers) -> %d%s\n", NS, (int)r,
           r==VK_SUCCESS?"":"  <-- REJECTED");
    if(r!=VK_SUCCESS){printf("VERDICT: driver rejects 32 samplers per stage\n");return 1;}

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp2; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp2),"cmdpool");
    VkCommandBufferAllocateInfo cba={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp2,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cba,&cb),"cmdbuf");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    for(int i=0;i<NS;i++){bar.image=img[i];vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&bar);}
    VkBufferImageCopy bic={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={0,0,0},.imageExtent={1,1,1}};
    for(int i=0;i<NS;i++){bic.bufferOffset=i*4; vkCmdCopyBufferToImage(cb,stage,img[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bic);}
    bar.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for(int i=0;i<NS;i++){bar.image=img[i];vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&bar);}
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pll,0,1,&ds,0,NULL);
    vkCmdDispatch(cb,1,1,1);
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");

    uint32_t*o=op; int good=0,zero=0,bad=0;
    for(int i=0;i<NS;i++){
        uint32_t want=(uint32_t)i|((uint32_t)i<<8)|((uint32_t)i<<16)|0xff000000u;
        if(o[i]==want)good++; else if(o[i]==0)zero++; else bad++;
    }
    printf("%d samplers: %d correct, %d untouched(0), %d wrong\n",NS,good,zero,bad);
    uint32_t probe[]={0,1,7,8,15,16,17,23,24,31};
    for(unsigned k=0;k<sizeof(probe)/sizeof(probe[0]);k++){
        uint32_t i=probe[k],want=i|(i<<8)|(i<<16)|0xff000000u;
        printf("  tex[%2u] %s got 0x%08x want 0x%08x\n",i,o[i]==want?"ok  ":"BAD ",o[i],want);
    }
    printf("VERDICT: %s (%d samplers per stage)\n",(good==NS)?"samplers work correctly":
                                  "samplers do NOT work", NS);
    return good==NS?0:1;
}
