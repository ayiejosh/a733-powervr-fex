/*
 * FOREIGN dma-buf test: validates the gem_prime_import FOREIGN path (the code
 * that was NOT covered by self-import tests) AND foreign-page GPU renderability.
 *
 * Source = /dev/dma_heap/system (NOT pvrsrvkm), so:
 *   (a) drmPrimeFDToHandle on renderD128 takes the FOREIGN branch
 *       (dma_buf_attach + PhysmemCreateNewDmaBufBackedPMR + wrap).
 *   (b) vkImportMemoryFdKHR on the same fd also takes PhysmemCreateNewDmaBufBackedPMR
 *       (services import, foreign), then GPU vkCmdFillBuffer -> CPU readback.
 *
 * Build: gcc dmabuf_foreign_test.c -o /tmp/dmabuf_foreign_test $(pkg-config --cflags --libs libdrm gbm) -lvulkan
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
#endif

#define SIZE (256*1024)
#define FILLVAL 0xCAFEF00Du
#define VKCHECK(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){fprintf(stderr,"FAIL %s=%d (line %d)\n",#x,_r,__LINE__);exit(2);} }while(0)

int main(void){
    /* ---- allocate a FOREIGN dma-buf from the system heap ---- */
    int heap = open("/dev/dma_heap/system", O_RDWR|O_CLOEXEC);
    if(heap<0){perror("open dma_heap/system");return 2;}
    struct dma_heap_allocation_data ad = { .len=SIZE, .fd_flags=O_RDWR|O_CLOEXEC };
    if(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &ad)){perror("DMA_HEAP_IOCTL_ALLOC");return 2;}
    int dfd = ad.fd;
    printf("foreign dma-buf from system heap: fd=%d size=%d\n", dfd, SIZE);

    /* ---- (a) FOREIGN gem path: drmPrimeFDToHandle on renderD128 ---- */
    int drmfd = open("/dev/dri/renderD128", O_RDWR|O_CLOEXEC);
    if(drmfd<0){perror("open renderD128");return 2;}
    uint32_t handle=0;
    int r = drmPrimeFDToHandle(drmfd, dfd, &handle);
    if(r){
        fprintf(stderr,"drmPrimeFDToHandle(FOREIGN) FAILED r=%d errno=%d (%s)\n", r, errno, strerror(errno));
        return 1;
    }
    printf("(a) drmPrimeFDToHandle(FOREIGN) OK -> handle=%u  [foreign gem path exercised]\n", handle);
    int reexport=-1;
    if(!drmPrimeHandleToFD(drmfd, handle, DRM_CLOEXEC, &reexport))
        printf("    re-export foreign handle->fd OK (%d)\n", reexport);

    /* ---- (b) prove foreign pages are GPU-renderable via Vulkan import+fill ---- */
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    const char* iext[]={VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=2,.ppEnabledExtensionNames=iext};
    VkInstance inst; VKCHECK(vkCreateInstance(&ici,NULL,&inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL); if(n>8)n=8;
    VkPhysicalDevice pd[8]; vkEnumeratePhysicalDevices(inst,&n,pd);
    VkPhysicalDevice phys=VK_NULL_HANDLE;
    for(uint32_t i=0;i<n;i++){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(pd[i],&p);if(strstr(p.deviceName,"PowerVR")||strstr(p.deviceName,"BXM"))phys=pd[i];}
    if(!phys){fprintf(stderr,"no PowerVR\n");return 2;}
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,NULL); if(nq>8)nq=8;
    VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,qf);
    uint32_t qfi=0; for(uint32_t i=0;i<nq;i++) if(qf[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_TRANSFER_BIT)){qfi=i;break;}
    float pr=1; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qfi,.queueCount=1,.pQueuePriorities=&pr};
    const char* dext[]={VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=3,.ppEnabledExtensionNames=dext};
    VkDevice dev; VKCHECK(vkCreateDevice(phys,&dci,NULL,&dev));
    VkQueue q; vkGetDeviceQueue(dev,qfi,0,&q);
    PFN_vkGetMemoryFdPropertiesKHR pGetFdProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev,"vkGetMemoryFdPropertiesKHR");

    VkExternalMemoryBufferCreateInfo eb={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&eb,.size=SIZE,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf; VKCHECK(vkCreateBuffer(dev,&bci,NULL,&buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,buf,&mr);
    VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    int impfd=dup(dfd);
    VKCHECK(pGetFdProps(dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,impfd,&fp));
    uint32_t bits=mr.memoryTypeBits&fp.memoryTypeBits;
    if(!bits){fprintf(stderr,"FAIL: no common memtype buf=0x%x dmabuf=0x%x\n",mr.memoryTypeBits,fp.memoryTypeBits);return 3;}
    uint32_t mt=__builtin_ctz(bits);
    printf("(b) vk import: bufBits=0x%x dmabufBits=0x%x memType=%u\n",mr.memoryTypeBits,fp.memoryTypeBits,mt);
    VkImportMemoryFdInfoKHR im={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=impfd};
    VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&im,.allocationSize=mr.size,.memoryTypeIndex=mt};
    VkDeviceMemory mem; VKCHECK(vkAllocateMemory(dev,&mai,NULL,&mem));
    VKCHECK(vkBindBufferMemory(dev,buf,mem,0));
    printf("    foreign dma-buf bound to VkBuffer OK\n");

    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=qfi};
    VkCommandPool cp; VKCHECK(vkCreateCommandPool(dev,&cpi,NULL,&cp));
    VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; VKCHECK(vkAllocateCommandBuffers(dev,&cbi,&cmd));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(cmd,&bi));
    vkCmdFillBuffer(cmd,buf,0,SIZE,FILLVAL);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    VKCHECK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(q));
    printf("    GPU vkCmdFillBuffer(0x%08X) on FOREIGN buffer done\n",FILLVAL);

    struct dma_buf_sync s={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ}; ioctl(dfd,DMA_BUF_IOCTL_SYNC,&s);
    void* map=mmap(NULL,SIZE,PROT_READ,MAP_SHARED,dfd,0);
    if(map==MAP_FAILED){perror("mmap foreign dma-buf");return 2;}
    volatile uint32_t* w=map; size_t nw=SIZE/4, ok=0; uint32_t fb=0; size_t fbi=0;
    for(size_t i=0;i<nw;i++){ if(w[i]==FILLVAL)ok++; else if(!fb){fb=w[i];fbi=i;} }
    struct dma_buf_sync e={.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ}; ioctl(dfd,DMA_BUF_IOCTL_SYNC,&e);
    printf("    readback: %zu/%zu == 0x%08X  sample=0x%08X 0x%08X\n",ok,nw,FILLVAL,w[0],w[1]);
    if(ok!=nw) printf("    first mismatch @%zu = 0x%08X\n",fbi,fb);

    if(ok==nw){ printf("\n=== PASS: FOREIGN dma-buf -- gem import OK + GPU renders foreign pages ===\n"); return 0; }
    printf("\n=== FAIL: foreign render mismatch (%zu/%zu) -- suspect resv/cache/lifetime ===\n",ok,nw);
    return 1;
}
