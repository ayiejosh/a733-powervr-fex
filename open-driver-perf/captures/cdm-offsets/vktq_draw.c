/* Minimal Vulkan compute draw: a compute shader rasterises a triangle into a 512x512 RGBA8 buffer.
 * Self-verifies the result, and is used as the working reference to capture a CDM job for replay. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
static void die(const char *w, VkResult r){fprintf(stderr,"%s failed %d\n",w,r);exit(1);}
static VkShaderModule load_shader(VkDevice dev, const char *path){
   FILE *f=fopen(path,"rb"); long sz; void *buf; VkShaderModule m; VkResult r;
   if(!f){perror(path);exit(1);} fseek(f,0,SEEK_END); sz=ftell(f); fseek(f,0,SEEK_SET);
   buf=malloc(sz); if(fread(buf,1,sz,f)!=(size_t)sz){exit(1);} fclose(f);
   VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sz,.pCode=buf};
   r=vkCreateShaderModule(dev,&ci,NULL,&m); if(r) die("vkCreateShaderModule",r); free(buf); return m;
}
int main(void){
   VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
   VkInstance inst; VkResult r=vkCreateInstance(&ici,NULL,&inst); if(r) die("instance",r);
   uint32_t n=1; VkPhysicalDevice phys; r=vkEnumeratePhysicalDevices(inst,&n,&phys); if(r||!n) die("phys",r);
   float prio=1.0f; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&prio};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
   VkDevice dev; r=vkCreateDevice(phys,&dci,NULL,&dev); if(r) die("dev",r);
   VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
   const unsigned W=512,H=512, SIZE=W*H*4;
   VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=SIZE,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkBuffer buf; r=vkCreateBuffer(dev,&bci,NULL,&buf); if(r) die("buf",r);
   VkMemoryRequirements mreq; vkGetBufferMemoryRequirements(dev,buf,&mreq);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys,&mp);
   uint32_t mt=~0u;
   for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((mreq.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) mt=i;
   VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mreq.size,.memoryTypeIndex=mt};
   VkDeviceMemory mem; r=vkAllocateMemory(dev,&mai,NULL,&mem); if(r) die("mem",r);
   vkBindBufferMemory(dev,buf,mem,0);
   void *map; vkMapMemory(dev,mem,0,SIZE,0,&map); memset(map,0,SIZE);
   VkDescriptorSetLayoutBinding b0={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dsl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&b0};
   VkDescriptorSetLayout layout; r=vkCreateDescriptorSetLayout(dev,&dsl,NULL,&layout); if(r) die("dsl",r);
   VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
   VkPipelineLayout playout; vkCreatePipelineLayout(dev,&plci,NULL,&playout);
   VkShaderModule sm=load_shader(dev,getenv("TRI_SPV")?getenv("TRI_SPV"):"tri.spv");
   VkComputePipelineCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=sm,.pName="main"},
      .layout=playout};
   VkPipeline pipe; r=vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpci,NULL,&pipe); if(r) die("pipe",r);
   VkDescriptorPoolSize psz={.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1};
   VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&psz};
   VkDescriptorPool pool; vkCreateDescriptorPool(dev,&dpci,NULL,&pool);
   VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
   VkDescriptorSet ds; vkAllocateDescriptorSets(dev,&dsai,&ds);
   VkDescriptorBufferInfo dbi={.buffer=buf,.offset=0,.range=SIZE};
   VkWriteDescriptorSet wds={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi};
   vkUpdateDescriptorSets(dev,1,&wds,0,NULL);
   VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool cpool; vkCreateCommandPool(dev,&pci,NULL,&cpool);
   VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cpool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer cb; vkAllocateCommandBuffers(dev,&cbi,&cb);
   VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   vkBeginCommandBuffer(cb,&bi);
   vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
   vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,playout,0,1,&ds,0,NULL);
   vkCmdDispatch(cb,64,64,1);
   vkEndCommandBuffer(cb);
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence; vkCreateFence(dev,&fci,NULL,&fence);
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
   r=vkQueueSubmit(q,1,&si,fence); if(r) die("submit",r);
   r=vkWaitForFences(dev,1,&fence,VK_TRUE,5000000000ull);
   if(r) die("wait",r);
   uint32_t *px=map; unsigned marker=0, okc=0, firstbad=~0u;
   for(unsigned i=0;i<W*H;i++) if(px[i]) marker++;
   if (px[0u] == 0xa0000000u) okc++; else if (firstbad==~0u) firstbad=0u;
   if (px[1u] == 0xa0000001u) okc++; else if (firstbad==~0u) firstbad=1u;
   if (px[2u] == 0xa0000002u) okc++; else if (firstbad==~0u) firstbad=2u;
   if (px[3u] == 0xa0000003u) okc++; else if (firstbad==~0u) firstbad=3u;
   if (px[4u] == 0xa0000004u) okc++; else if (firstbad==~0u) firstbad=4u;
   if (px[8u] == 0xa0000005u) okc++; else if (firstbad==~0u) firstbad=8u;
   if (px[16u] == 0xa0000006u) okc++; else if (firstbad==~0u) firstbad=16u;
   if (px[32u] == 0xa0000007u) okc++; else if (firstbad==~0u) firstbad=32u;
   if (px[64u] == 0xa0000008u) okc++; else if (firstbad==~0u) firstbad=64u;
   if (px[100u] == 0xa0000009u) okc++; else if (firstbad==~0u) firstbad=100u;
   if (px[128u] == 0xa000000au) okc++; else if (firstbad==~0u) firstbad=128u;
   if (px[256u] == 0xa000000bu) okc++; else if (firstbad==~0u) firstbad=256u;
   if (px[512u] == 0xa000000cu) okc++; else if (firstbad==~0u) firstbad=512u;
   if (px[1024u] == 0xa000000du) okc++; else if (firstbad==~0u) firstbad=1024u;
   if (px[4096u] == 0xa000000eu) okc++; else if (firstbad==~0u) firstbad=4096u;
   printf("offsets probe: nonzero=%u matched=%u/%u firstbad=%u\n", marker, okc, 15u, firstbad);
   printf("RESULT: %s\n", (okc==15u) ? "PASS" : "FAIL");
   vkUnmapMemory(dev,mem); vkDestroyFence(dev,fence,0); vkDestroyCommandPool(dev,cpool,0);
   vkDestroyDescriptorPool(dev,pool,0); vkDestroyPipeline(dev,pipe,0); vkDestroyPipelineLayout(dev,playout,0);
   vkDestroyDescriptorSetLayout(dev,layout,0); vkDestroyShaderModule(dev,sm,0); vkDestroyBuffer(dev,buf,0);
   vkFreeMemory(dev,mem,0); vkDestroyDevice(dev,0); vkDestroyInstance(inst,0);
   return 0;
}
