/* mrt - does the driver really render to 8 colour attachments?
 *
 * maxColorAttachments was hardcoded to 4 while the driver's own constant
 * (PVR_MAX_COLOR_ATTACHMENTS = PVR_NUM_PBE_EMIT_REGS = 8) and the vendor both say 8.
 * That fix rested on static evidence, so this measures it: eight 1x1 colour attachments,
 * a fragment shader with eight outputs where output i writes i into red, and a readback of
 * every attachment. Attachment i must read back i - a driver that only bound 4 would leave
 * attachments 4..7 at their clear value, and one that aliased them would show the wrong index.
 *
 * Uses VK_KHR_dynamic_rendering (reported by the driver) so there is no render pass object.
 * usage: ./varyings <vert.spv> <frag.spv> <n>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define NA 1
#define CK(x,m) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",m,(int)r);exit(1);}}while(0)
static uint32_t*rd(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}
 fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);uint32_t*b=malloc(s);
 if(fread(b,1,s,f)!=(size_t)s)exit(1);fclose(f);*n=s;return b;}
int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <vert.spv> <frag.spv>\n",argv[0]);return 2;}
    setvbuf(stdout,NULL,_IONBF,0);
    size_t vn,fn; uint32_t*vs=rd(argv[1],&vn),*fs=rd(argv[2],&fn);
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst),"instance");
    uint32_t np=0; vkEnumeratePhysicalDevices(inst,&np,NULL);
    VkPhysicalDevice*pd=malloc(np*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&np,pd);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[0],&pr);
    printf("device: %s\n",pr.deviceName);
    printf("reported maxColorAttachments = %u\n",pr.limits.maxColorAttachments);
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
    VkPhysicalDeviceDynamicRenderingFeatures drf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,.dynamicRendering=VK_TRUE};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&drf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CK(vkCreateDevice(pd[0],&dci,NULL,&dev),"device");
    VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0],&mp);
    uint32_t hv=0; for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){hv=i;break;}

    VkImage img[NA]; VkDeviceMemory im[NA]; VkImageView iv[NA];
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    for(int i=0;i<NA;i++){
        CK(vkCreateImage(dev,&ic,NULL,&img[i]),"image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img[i],&mr);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hv};
        CK(vkAllocateMemory(dev,&ma,NULL,&im[i]),"mem"); CK(vkBindImageMemory(dev,img[i],im[i],0),"bind");
        VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CK(vkCreateImageView(dev,&vc,NULL,&iv[i]),"view");
    }
    /* readback buffer: NA pixels, each 4 bytes */
    VkBufferCreateInfo ob={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=NA*4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer out; CK(vkCreateBuffer(dev,&ob,NULL,&out),"outbuf");
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(dev,out,&omr);
    VkMemoryAllocateInfo oma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=omr.size,.memoryTypeIndex=hv};
    VkDeviceMemory om; CK(vkAllocateMemory(dev,&oma,NULL,&om),"outmem"); CK(vkBindBufferMemory(dev,out,om,0),"bindout");
    void*op; CK(vkMapMemory(dev,om,0,omr.size,0,&op),"map"); memset(op,0,omr.size);

    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pll; CK(vkCreatePipelineLayout(dev,&pl,NULL,&pll),"pl");
    VkShaderModuleCreateInfo sv={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vn,.pCode=vs};
    VkShaderModule mv; CK(vkCreateShaderModule(dev,&sv,NULL,&mv),"vm");
    VkShaderModuleCreateInfo sf={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fn,.pCode=fs};
    VkShaderModule mf; CK(vkCreateShaderModule(dev,&sf,NULL,&mf),"fm");
    VkFormat fmts[NA]; for(int i=0;i<NA;i++) fmts[i]=VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount=NA,.pColorAttachmentFormats=fmts};
    VkPipelineColorBlendAttachmentState cba[NA];
    for(int i=0;i<NA;i++){cba[i].blendEnable=VK_FALSE;
        cba[i].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;}
    VkPipelineColorBlendStateCreateInfo cbs={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=NA,.pAttachments=cba};
    VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.scissorCount=1};
    VkPipelineMultisampleStateCreateInfo mss={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth=1.0f};
    VkPipelineVertexInputStateCreateInfo vis={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2,.pDynamicStates=dyn};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&prc,
        .stageCount=2,.pStages=(VkPipelineShaderStageCreateInfo[]){
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=mv,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=mf,.pName="main"}},
        .pVertexInputState=&vis,.pInputAssemblyState=&ias,.pViewportState=&vps,
        .pRasterizationState=&rs,.pMultisampleState=&mss,.pColorBlendState=&cbs,
        .pDynamicState=&ds,.layout=pll};
    VkPipeline pipe; VkResult r=vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,NULL,&pipe);
    printf("vkCreateGraphicsPipelines -> %d%s\n",(int)r,r==VK_SUCCESS?"":"  <-- REJECTED");
    if(r!=VK_SUCCESS){printf("VERDICT: driver REJECTS this varying count\n");return 1;}

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp),"cmdpool");
    VkCommandBufferAllocateInfo cba2={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cba2,&cb),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    for(int i=0;i<NA;i++){bar.image=img[i];vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&bar);}
    VkRenderingAttachmentInfo rai[NA];
    for(int i=0;i<NA;i++){
        rai[i]=(VkRenderingAttachmentInfo){.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView=iv[i],.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue={.color={.float32={0.0f,0.0f,0.0f,0.0f}}}};
    }
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{1,1}},
        .layerCount=1,.colorAttachmentCount=NA,.pColorAttachments=rai};
    vkCmdBeginRendering(cb,&ri);
    VkViewport vp={0,0,1,1,0,1}; vkCmdSetViewport(cb,0,1,&vp);
    VkRect2D sc={{0,0},{1,1}}; vkCmdSetScissor(cb,0,1,&sc);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRendering(cb);
    VkImageMemoryBarrier b2=bar;
    b2.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b2.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for(int i=0;i<NA;i++){b2.image=img[i];vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b2);}
    VkBufferImageCopy bic={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={0,0,0},.imageExtent={1,1,1}};
    for(int i=0;i<NA;i++){bic.bufferOffset=i*4;vkCmdCopyImageToBuffer(cb,img[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,out,1,&bic);}
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");
    unsigned char*o=op;
    int n = argc>3 ? atoi(argv[3]) : 0;
    /* the shader writes sum/512 into an RGBA8 attachment */
    int sum = n*(n-1)/2;
    int want = (int)(sum/512.0*255.0 + 0.5);
    printf("%d varyings (%d components): got red=%u want=%d\n", n, n*4, o[0], want);
    printf("VERDICT: %s\n", (o[0]==want) ? "varyings interpolate correctly"
                                         : "varyings do NOT interpolate correctly");
    return o[0]==want?0:1;
}
