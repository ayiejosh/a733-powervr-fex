/* Minimal Vulkan TQ copy test: fill a source buffer with a pattern, vkCmdCopyBuffer to a second
 * buffer, submit, wait, verify. Prints both buffer device addresses so the captured transfer stream
 * can be decoded for its source/destination address fields. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define SIZE_DEFAULT (64u * 64u * 4u)
#define PATTERN 0xdeadbeefu

static void die(const char *what, VkResult r) { fprintf(stderr, "%s failed: %d\n", what, r); exit(1); }

int main(void)
{
   const unsigned size = getenv("FILL_BYTES") ? (unsigned)strtoul(getenv("FILL_BYTES"), NULL, 0) : SIZE_DEFAULT;
   const unsigned pattern = getenv("FILL_VALUE") ? (unsigned)strtoul(getenv("FILL_VALUE"), NULL, 0) : PATTERN;
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_0 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
   VkInstance inst;
   uint32_t count = 1;
   VkPhysicalDevice phys;
   VkResult r;

   r = vkCreateInstance(&ici, NULL, &inst); if (r) die("vkCreateInstance", r);
   r = vkEnumeratePhysicalDevices(inst, &count, &phys); if (r || !count) die("vkEnumeratePhysicalDevices", r);
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
   printf("device: %s\n", props.deviceName);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   static const char *dev_exts[] = { VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME };
   VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
      .bufferDeviceAddress = VK_TRUE,
   };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &bda,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts };
   VkDevice dev;
   r = vkCreateDevice(phys, &dci, NULL, &dev); if (r) die("vkCreateDevice", r);
   VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);

   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
   VkBuffer buf, dst; r = vkCreateBuffer(dev, &bci, NULL, &buf); if (r) die("vkCreateBuffer src", r);
   r = vkCreateBuffer(dev, &bci, NULL, &dst); if (r) die("vkCreateBuffer dst", r);
   {
      PFN_vkGetBufferDeviceAddressKHR get_bda =
         (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddressKHR");
      if (get_bda) {
         VkBufferDeviceAddressInfo bdai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                            .buffer = buf };
         VkDeviceAddress a = get_bda(dev, &bdai);
         bdai.buffer = dst;
         VkDeviceAddress b = get_bda(dev, &bdai);
         printf("src device address: 0x%llx dst device address: 0x%llx\n",
                (unsigned long long)a, (unsigned long long)b);
      } else {
         printf("vkGetBufferDeviceAddressKHR not available\n");
      }
   }
   VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
   VkMemoryRequirements dst_req; vkGetBufferMemoryRequirements(dev, dst, &dst_req);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   uint32_t type = ~0u;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         type = i;
   if (type == ~0u) { fprintf(stderr, "no host-visible memory type\n"); return 1; }
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size, .memoryTypeIndex = type };
   VkDeviceMemory mem; r = vkAllocateMemory(dev, &mai, NULL, &mem); if (r) die("vkAllocateMemory src", r);
   r = vkBindBufferMemory(dev, buf, mem, 0); if (r) die("vkBindBufferMemory src", r);
   VkDeviceMemory dmem; r = vkAllocateMemory(dev, &mai, NULL, &dmem); if (r) die("vkAllocateMemory dst", r);
   r = vkBindBufferMemory(dev, dst, dmem, 0); if (r) die("vkBindBufferMemory dst", r);

   void *map; r = vkMapMemory(dev, mem, 0, size, 0, &map); if (r) die("vkMapMemory src", r);
   memset(map, 0, size);
   void *dmap; r = vkMapMemory(dev, dmem, 0, size, 0, &dmap); if (r) die("vkMapMemory dst", r);
   memset(dmap, 0, size);

   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool; r = vkCreateCommandPool(dev, &pci, NULL, &pool); if (r) die("vkCreateCommandPool", r);
   VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
   VkCommandBuffer cb; r = vkAllocateCommandBuffers(dev, &cbi, &cb); if (r) die("vkAllocateCommandBuffers", r);

   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cb, &bi);
   vkCmdFillBuffer(cb, buf, 0, size, pattern);
   VkBufferMemoryBarrier bmb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .buffer = buf, .offset = 0, .size = size };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        0, NULL, 1, &bmb, 0, NULL);
   VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = size };
   vkCmdCopyBuffer(cb, buf, dst, 1, &region);
   vkEndCommandBuffer(cb);

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence; r = vkCreateFence(dev, &fci, NULL, &fence); if (r) die("vkCreateFence", r);
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1, .pCommandBuffers = &cb };
   r = vkQueueSubmit(queue, 1, &si, fence); if (r) die("vkQueueSubmit", r);
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
   printf("fence wait: %d\n", r);
   if (r == VK_SUCCESS) {
      int bad = 0, changed = 0;
      volatile uint32_t *px = map;
      volatile uint32_t *dx = dmap;
      for (unsigned i = 0; i < size / 4; i++) {
         if (px[i] == pattern) changed = 1;
         if (dx[i] != pattern) bad++;
      }
      printf("copy: %u dwords, src filled=%d, dst mismatched=%d, dst[0]=0x%08x\n",
             size / 4, changed, bad, dx[0]);
      printf("RESULT: %s\n", (changed && bad == 0) ? "PASS" : "FAIL");
      vkUnmapMemory(dev, dmem);
      vkUnmapMemory(dev, mem);
   }
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buf, NULL);
   vkDestroyBuffer(dev, dst, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkFreeMemory(dev, dmem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
