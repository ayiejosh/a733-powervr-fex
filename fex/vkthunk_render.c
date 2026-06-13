/* Pure-Vulkan data-path test for the FEX Vulkan thunk (no gbm, no surface).
 * HOST_VISIBLE|HOST_COHERENT buffer -> GPU vkCmdFillBuffer(0xDEADBEEF) -> submit
 * -> vkMapMemory -> verify bytes. Exercises memory alloc/map + command submit
 * THROUGH the thunk (the part vulkaninfo never touches).
 * Cross-compile x86: clang-19 --target=x86_64-linux-gnu --sysroot=/home/radxa/crd-rootfs
 *   -I FEX-src/External/Vulkan-Headers/include vkthunk_render.c -o vkthunk_render -l:libvulkan.so.1 -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define SZ (64*1024)
#define FILLVAL 0xDEADBEEFu
#define CK(x) do{VkResult r=(x); if(r!=VK_SUCCESS){fprintf(stderr,"FAIL %s=%d L%d\n",#x,r,__LINE__);return 2;}}while(0)

int main(void){
  VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
  VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
  VkInstance inst; CK(vkCreateInstance(&ic,0,&inst));
  uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0); if(!n){fprintf(stderr,"no devices\n");return 2;}
  if(n>8)n=8; VkPhysicalDevice pd[8]; vkEnumeratePhysicalDevices(inst,&n,pd);
  VkPhysicalDevice phys=0; VkPhysicalDeviceProperties props;
  for(uint32_t i=0;i<n;i++){vkGetPhysicalDeviceProperties(pd[i],&props); if(strstr(props.deviceName,"PowerVR")||strstr(props.deviceName,"BXM")){phys=pd[i];break;}}
  if(!phys){phys=pd[0];vkGetPhysicalDeviceProperties(phys,&props);}
  printf("device: %s\n", props.deviceName);

  uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,0); if(qn>8)qn=8;
  VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qf);
  uint32_t qi=0; for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_TRANSFER_BIT|VK_QUEUE_COMPUTE_BIT)){qi=i;break;}
  float pr=1; VkDeviceQueueCreateInfo qc={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&pr};
  VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qc};
  VkDevice dev; CK(vkCreateDevice(phys,&dc,0,&dev));
  VkQueue q; vkGetDeviceQueue(dev,qi,0,&q);

  VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=SZ,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  VkBuffer buf; CK(vkCreateBuffer(dev,&bc,0,&buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,buf,&mr);
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys,&mp);
  uint32_t mt=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;i++){
    if((mr.memoryTypeBits&(1u<<i)) &&
       (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
       (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){mt=i;break;}}
  if(mt==UINT32_MAX){fprintf(stderr,"no HOST_VISIBLE|COHERENT memtype\n");return 2;}
  VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};
  VkDeviceMemory mem; CK(vkAllocateMemory(dev,&ai,0,&mem));
  CK(vkBindBufferMemory(dev,buf,mem,0));
  printf("alloc+bind HOST_VISIBLE memtype %u OK\n", mt);

  VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=qi};
  VkCommandPool pool; CK(vkCreateCommandPool(dev,&pc,0,&pool));
  VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
  VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev,&ca,&cmd));
  VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  CK(vkBeginCommandBuffer(cmd,&bi));
  vkCmdFillBuffer(cmd,buf,0,SZ,FILLVAL);
  VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,0,0,0);
  CK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
  CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE));
  CK(vkQueueWaitIdle(q));
  printf("GPU vkCmdFillBuffer(0x%08X) submitted + completed\n", FILLVAL);

  void* p=0; CK(vkMapMemory(dev,mem,0,SZ,0,&p));
  volatile uint32_t* w=p; size_t nw=SZ/4, ok=0; uint32_t fb=0; size_t fbi=0;
  for(size_t i=0;i<nw;i++){ if(w[i]==FILLVAL)ok++; else if(!fb){fb=w[i];fbi=i;} }
  printf("readback: %zu/%zu == 0x%08X  sample[0..2]=0x%08X 0x%08X 0x%08X\n",ok,nw,FILLVAL,w[0],w[1],w[2]);
  if(ok!=nw) printf("first mismatch @%zu = 0x%08X\n",fbi,fb);
  vkUnmapMemory(dev,mem);
  if(ok==nw){ printf("\n=== PASS: GPU rendered + mapped-readback correct THROUGH THE THUNK ===\n"); return 0; }
  printf("\n=== FAIL: data-path through thunk produced wrong bytes (%zu/%zu) ===\n",ok,nw); return 1;
}
