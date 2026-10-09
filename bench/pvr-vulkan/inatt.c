/* inatt - probe maxPerStageDescriptorInputAttachments.
 *
 * The driver reports 4 (the Vulkan minimum) with no backing constant, so it may be a floor.
 * A 2-subpass render pass: subpass 0 writes a distinct value into each of N colour attachments;
 * subpass 1 reads all N as input attachments, sums them and writes the sum. The readback must
 * equal the expected sum, so a dropped input attachment shows a wrong number, not a plausible one.
 *
 * usage: ./inatt <vert.spv> <frag.spv> <n>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x,m) do{VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s -> %d\n",m,(int)r);exit(1);}}while(0)
static uint32_t*rd(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}
 fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);uint32_t*b=malloc(s);
 if(fread(b,1,s,f)!=(size_t)s)exit(1);fclose(f);*n=s;return b;}
int main(int argc,char**argv){
    if(argc<5){fprintf(stderr,"usage: %s <vert.spv> <subpass0.frag.spv> <subpass1.frag.spv> <n>\n",argv[0]);return 2;}
    int N=atoi(argv[4]);
    setvbuf(stdout,NULL,_IONBF,0);
    size_t vn,f0n,f1n; uint32_t*vs=rd(argv[1],&vn),*f0=rd(argv[2],&f0n),*fs=rd(argv[3],&f1n);
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst),"instance");
    uint32_t np=0; vkEnumeratePhysicalDevices(inst,&np,NULL);
    VkPhysicalDevice*pd=malloc(np*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&np,pd);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[0],&pr);
    printf("device: %s\n",pr.deviceName);
    printf("reported maxPerStageDescriptorInputAttachments = %u\n",pr.limits.maxPerStageDescriptorInputAttachments);
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice dev; CK(vkCreateDevice(pd[0],&dci,NULL,&dev),"device");
    VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[0],&mp);
    uint32_t hv=0; for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){hv=i;break;}
    /* N attachments + 1 output attachment */
    int NT=N+1;
    VkImage img[64]; VkDeviceMemory im[64]; VkImageView iv[64];
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    for(int i=0;i<NT;i++){
        CK(vkCreateImage(dev,&ic,NULL,&img[i]),"image");
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img[i],&mr);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=hv};
        CK(vkAllocateMemory(dev,&ma,NULL,&im[i]),"mem"); CK(vkBindImageMemory(dev,img[i],im[i],0),"bind");
        VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CK(vkCreateImageView(dev,&vc,NULL,&iv[i]),"view");
    }
    /* render pass: subpass 0 -> N attachments; subpass 1 -> N input attachments + output */
    VkAttachmentDescription ad[64];
    for(int i=0;i<NT;i++) ad[i]=(VkAttachmentDescription){.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color0[64];
    for(int i=0;i<N;i++) color0[i]=(VkAttachmentReference){i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference inref[64];
    for(int i=0;i<N;i++) inref[i]=(VkAttachmentReference){i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference outref={N,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp[2]={
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=N,
         .pColorAttachments=color0},
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.inputAttachmentCount=N,
         .pInputAttachments=inref,.colorAttachmentCount=1,.pColorAttachments=&outref},
    };
    VkSubpassDependency dep={
        .srcSubpass=0,.dstSubpass=1,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=NT,.pAttachments=ad,.subpassCount=2,.pSubpasses=sp,
        .dependencyCount=1,.pDependencies=&dep};
    VkRenderPass rp; VkResult rr=vkCreateRenderPass(dev,&rpci,NULL,&rp);
    printf("vkCreateRenderPass with %d input attachments -> %d%s\n",N,(int)rr,rr==VK_SUCCESS?"":"  <-- REJECTED");
    if(rr!=VK_SUCCESS){printf("VERDICT: driver rejects %d input attachments\\n",N);return 1;}
    VkFramebufferCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,
        .attachmentCount=NT,.pAttachments=iv,.width=1,.height=1,.layers=1};
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev,&fci,NULL,&fb),"framebuffer");
    /* descriptor set: N input attachments */
    VkDescriptorSetLayoutBinding b[64];
    for(int i=0;i<N;i++) b[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=N,.pBindings=b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev,&dl,NULL,&dsl),"dsl");
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,N};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev,&dp,NULL,&pool),"pool");
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&da,&ds),"set");
    VkDescriptorImageInfo dii[64]; VkWriteDescriptorSet w[64];
    for(int i=0;i<N;i++){
        dii[i]=(VkDescriptorImageInfo){.imageView=iv[i],.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,
            .dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo=&dii[i]};
    }
    vkUpdateDescriptorSets(dev,N,w,0,NULL);
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    VkPipelineLayout pll; CK(vkCreatePipelineLayout(dev,&pl,NULL,&pll),"pl");
    VkShaderModuleCreateInfo sv={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vn,.pCode=vs};
    VkShaderModule mv; CK(vkCreateShaderModule(dev,&sv,NULL,&mv),"vm");
    VkShaderModuleCreateInfo sf0={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=f0n,.pCode=f0};
    VkShaderModule mf0; CK(vkCreateShaderModule(dev,&sf0,NULL,&mf0),"fm0");
    VkShaderModuleCreateInfo sf={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=f1n,.pCode=fs};
    VkShaderModule mf; CK(vkCreateShaderModule(dev,&sf,NULL,&mf),"fm");
    VkPipelineShaderStageCreateInfo st0[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=mv,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=mf0,.pName="main"}};
    VkPipelineShaderStageCreateInfo st1[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=mv,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=mf,.pName="main"}};
    VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineMultisampleStateCreateInfo mss={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
    VkPipelineVertexInputStateCreateInfo vis={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsst={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dyn};
    VkPipelineColorBlendAttachmentState cba[64];
    for(int i=0;i<N;i++){cba[i].blendEnable=VK_FALSE;cba[i].colorWriteMask=0xf;}
    VkPipelineColorBlendAttachmentState cba1={.blendEnable=VK_FALSE,.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cbs0={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=N,.pAttachments=cba};
    VkPipelineColorBlendStateCreateInfo cbs1={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba1};
    VkGraphicsPipelineCreateInfo gp[2]={
        {.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st0,
         .pVertexInputState=&vis,.pInputAssemblyState=&ias,.pViewportState=&vps,.pRasterizationState=&rs,
         .pMultisampleState=&mss,.pColorBlendState=&cbs0,.pDynamicState=&dsst,.layout=pll,.renderPass=rp,.subpass=0},
        {.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st1,
         .pVertexInputState=&vis,.pInputAssemblyState=&ias,.pViewportState=&vps,.pRasterizationState=&rs,
         .pMultisampleState=&mss,.pColorBlendState=&cbs1,.pDynamicState=&dsst,.layout=pll,.renderPass=rp,.subpass=1}};
    VkPipeline pipe[2];
    VkResult pr2=vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,2,gp,NULL,pipe);
    printf("vkCreateGraphicsPipelines (subpass 1 uses %d input attachments) -> %d%s\n",N,(int)pr2,
           pr2==VK_SUCCESS?"":"  <-- REJECTED");
    if(pr2!=VK_SUCCESS){printf("VERDICT: driver rejects %d input attachments\\n",N);return 1;}
    /* record + render */
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp),"cmdpool");
    VkCommandBufferAllocateInfo cbaa={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbaa,&cb),"cb");
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb,&bi),"begin");
    VkClearValue cv[64]; for(int i=0;i<NT;i++) cv[i].color.float32[0]=0.0f;
    VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,
        .framebuffer=fb,.renderArea={{0,0},{1,1}},.clearValueCount=NT,.pClearValues=cv};
    vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp={0,0,1,1,0,1}; vkCmdSetViewport(cb,0,1,&vp);
    VkRect2D sc={{0,0},{1,1}}; vkCmdSetScissor(cb,0,1,&sc);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe[0]);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdNextSubpass(cb,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe[1]);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pll,0,1,&ds,0,NULL);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRenderPass(cb);
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},.image=img[N]};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&bar);
    VkBufferCreateInfo ob={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer out; CK(vkCreateBuffer(dev,&ob,NULL,&out),"outbuf");
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(dev,out,&omr);
    VkMemoryAllocateInfo oma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=omr.size,.memoryTypeIndex=hv};
    VkDeviceMemory om; CK(vkAllocateMemory(dev,&oma,NULL,&om),"outmem"); CK(vkBindBufferMemory(dev,out,om,0),"bindout");
    void*op; CK(vkMapMemory(dev,om,0,omr.size,0,&op),"map"); memset(op,0,omr.size);
    VkBufferImageCopy bic={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={0,0,0},.imageExtent={1,1,1}};
    vkCmdCopyImageToBuffer(cb,img[N],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,out,1,&bic);
    CK(vkEndCommandBuffer(cb),"end");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"submit"); CK(vkQueueWaitIdle(q),"wait");
    unsigned char*o=op;
    /* subpass 0 writes 1/255 into attachment i, so the sum is N/255 -> red = N (mod 256) */
    int want=N%256;
    printf("%d input attachments: got red=%u want=%d\n",N,o[0],want);
    printf("VERDICT: %s\n",(o[0]==want)?"input attachments work correctly":
                                  "input attachments do NOT work correctly");
    return o[0]==want?0:1;
}
