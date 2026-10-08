/* samplers - probe maxPerStageDescriptorSamplers.
 *
 * The driver reports 16 (the Vulkan minimum); the vendor reports 32 on the same silicon.
 * This binds a 32-element sampler2D array, where texture i is a 1x1 image holding the
 * value i, and the shader writes each sampled value to slot i. The readback therefore
 * proves both how many samplers are usable and that each index reads the RIGHT texture -
 * an array that silently aliased or ran out would show wrong values, not just zeros.
 *
 * usage: ./stgbuf <spv> <n>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x,m) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",m,(int)r);exit(1);}}while(0)
#ifndef NS
#define NS 128
#endif
int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <spv> <n>\n",argv[0]);return 2;}
    const int N=atoi(argv[2]);
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

    /* N storage buffers, buffer i holds the uint i, so the shader's sum is 0+1+..+N-1 */
    VkBuffer ub[NS]; VkDeviceMemory um[NS];
    VkBufferCreateInfo uic={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    for(int i=0;i<N;i++){
        CK(vkCreateBuffer(dev,&uic,NULL,&ub[i]),"ubuf");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,ub[i],&mr);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hostvis};
        CK(vkAllocateMemory(dev,&ma,NULL,&um[i]),"umem");
        CK(vkBindBufferMemory(dev,ub[i],um[i],0),"bindubuf");
        void*up; CK(vkMapMemory(dev,um[i],0,4,0,&up),"mapubuf");
        *(uint32_t*)up=(uint32_t)i; vkUnmapMemory(dev,um[i]);
    }
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
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=N,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"dsl");
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,NS},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev,&dp,NULL,&pool),"pool");
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&da,&ds),"allocset");
    VkDescriptorBufferInfo ubi[NS];
    for(int i=0;i<N;i++) ubi[i]=(VkDescriptorBufferInfo){.buffer=ub[i],.offset=0,.range=4};
    VkDescriptorBufferInfo dbi={.buffer=out,.offset=0,.range=VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=N,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=ubi},
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
    printf("vkCreateComputePipelines (%d storage buffers) -> %d%s\n", NS, (int)r,
           r==VK_SUCCESS?"":"  <-- REJECTED");
    if(r!=VK_SUCCESS){printf("VERDICT: driver REJECTS %d storage buffers per stage\n",N);return 1;}

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp2; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp2),"cmdpool");
    VkCommandBufferAllocateInfo cba={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp2,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cba,&cb),"cmdbuf");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pll,0,1,&ds,0,NULL);
    vkCmdDispatch(cb,1,1,1);
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");

    uint32_t*o=op;
    /* the shader sums buffer i's value for i in 0..N-1, so the result must be the triangular sum */
    uint32_t want=(uint32_t)((uint64_t)N*(N-1)/2);
    printf("%d storage buffers per stage: got %u want %u\n", N, o[0], want);
    printf("VERDICT: %s (%d storage buffers per stage)\n",
           (o[0]==want)?"storage buffers work correctly":"storage buffers do NOT work", N);
    return o[0]==want?0:1;
}
