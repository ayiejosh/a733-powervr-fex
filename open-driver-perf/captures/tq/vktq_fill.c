/* Minimal Vulkan TQ fill test: create a buffer, zero it, vkCmdFillBuffer with a known pattern,
 * submit, wait, and verify the buffer contents. Used to capture a working TQ (transfer) job. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define SIZE (64u * 64u * 4u)
#define PATTERN 0xdeadbeefu

static void die(const char *what, VkResult r) { fprintf(stderr, "%s failed: %d\n", what, r); exit(1); }

int main(void)
{
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
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev;
   r = vkCreateDevice(phys, &dci, NULL, &dev); if (r) die("vkCreateDevice", r);
   VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);

   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
   VkBuffer buf; r = vkCreateBuffer(dev, &bci, NULL, &buf); if (r) die("vkCreateBuffer", r);
   VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   uint32_t type = ~0u;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         type = i;
   if (type == ~0u) { fprintf(stderr, "no host-visible memory type\n"); return 1; }
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size, .memoryTypeIndex = type };
   VkDeviceMemory mem; r = vkAllocateMemory(dev, &mai, NULL, &mem); if (r) die("vkAllocateMemory", r);
   r = vkBindBufferMemory(dev, buf, mem, 0); if (r) die("vkBindBufferMemory", r);

   void *map; r = vkMapMemory(dev, mem, 0, SIZE, 0, &map); if (r) die("vkMapMemory", r);
   memset(map, 0, SIZE);

   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool; r = vkCreateCommandPool(dev, &pci, NULL, &pool); if (r) die("vkCreateCommandPool", r);
   VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
   VkCommandBuffer cb; r = vkAllocateCommandBuffers(dev, &cbi, &cb); if (r) die("vkAllocateCommandBuffers", r);

   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cb, &bi);
   vkCmdFillBuffer(cb, buf, 0, SIZE, PATTERN);
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
      for (unsigned i = 0; i < SIZE / 4; i++) {
         if (px[i] != 0) changed = 1;
         if (px[i] != PATTERN) bad++;
      }
      printf("fill: %u dwords, changed=%d, mismatched=%d, first=0x%08x\n",
             SIZE / 4, changed, bad, px[0]);
      printf("RESULT: %s\n", (changed && bad == 0) ? "PASS" : "FAIL");
      vkUnmapMemory(dev, mem);
   }
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
