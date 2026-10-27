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
   uint32_t *px=map; unsigned red=0, blue=0, bad=0;
   for(unsigned i=1;i<W*H;i++){ if(px[i]==0xFF0000FFu) red++; else if(px[i]==0xFFFF0000u) blue++; else if(px[i]) bad++; }
   printf("triangle: red=%u blue=%u bad=%u marker=%08x\n", red, blue, bad, px[0]);
   printf("RESULT: %s\n", (bad==0 && red>1000 && blue>1000 && px[0]==0xCAFEBABEu) ? "PASS" : "FAIL");
   vkUnmapMemory(dev,mem); vkDestroyFence(dev,fence,0); vkDestroyCommandPool(dev,cpool,0);
   vkDestroyDescriptorPool(dev,pool,0); vkDestroyPipeline(dev,pipe,0); vkDestroyPipelineLayout(dev,playout,0);
   vkDestroyDescriptorSetLayout(dev,layout,0); vkDestroyShaderModule(dev,sm,0); vkDestroyBuffer(dev,buf,0);
   vkFreeMemory(dev,mem,0); vkDestroyDevice(dev,0); vkDestroyInstance(inst,0);
   return 0;
}
