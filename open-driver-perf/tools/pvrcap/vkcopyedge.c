/* vkcopyedge: a small vkCmdCopyBuffer whose destination range crosses an 8 KiB row boundary and ends at the end of a
 * buffer whose memory is barely larger than the buffer. Checks the copied bytes and that bytes around the copy are
 * untouched. Run with the firmware trace on to see a GPU page fault. Exit 0 = data correct. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   printf("no memory type\n"); exit(2);
}
static void mkbuf(VkDeviceSize size, VkBuffer *b, VkDeviceMemory *m, void **map) {
   VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
   CK(vkCreateBuffer(dev, &bi, NULL, b));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   CK(vkAllocateMemory(dev, &ai, NULL, m)); CK(vkBindBufferMemory(dev, *b, *m, 0)); CK(vkMapMemory(dev, *m, 0, size, 0, map));
   printf("buffer size %llu, memory %llu\n", (unsigned long long)size, (unsigned long long)mr.size);
}
int main(int argc, char **argv) {
   const VkDeviceSize DST = argc > 1 ? strtoull(argv[1], NULL, 0) : 8392, LEN = 400, DOFF = DST - LEN;
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0);
   float pr = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pr };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   CK(vkCreateDevice(pd, &di, NULL, &dev)); VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
   VkBuffer sb, db; VkDeviceMemory sm, dm; void *sp, *dp;
   mkbuf(LEN, &sb, &sm, &sp); mkbuf(DST, &db, &dm, &dp);
   for (VkDeviceSize i = 0; i < LEN; i++) ((uint8_t *)sp)[i] = (uint8_t)(i * 7 + 1);
   memset(dp, 0xa5, DST);
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CK(vkBeginCommandBuffer(cb, &bgi));
   VkBufferCopy rg = { .srcOffset = 0, .dstOffset = DOFF, .size = LEN };
   vkCmdCopyBuffer(cb, sb, db, 1, &rg);
   CK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   VkResult w = vkQueueWaitIdle(q);
   printf("copy %llu bytes to offset %llu (crosses the 8 KiB row at 8192: %s); wait %d\n", (unsigned long long)LEN,
          (unsigned long long)DOFF, DOFF < 8192 && DOFF + LEN > 8192 ? "yes" : "no", w);
   int bad = 0;
   for (VkDeviceSize i = 0; i < DST; i++) {
      uint8_t want = (i >= DOFF && i < DOFF + LEN) ? (uint8_t)((i - DOFF) * 7 + 1) : 0xa5;
      if (((uint8_t *)dp)[i] != want) { if (bad < 5) printf("byte %llu = 0x%02x, want 0x%02x\n", (unsigned long long)i, ((uint8_t *)dp)[i], want); bad++; }
   }
   printf("%s: %d wrong bytes\n", bad || w ? "FAIL" : "PASS", bad);
   return bad || w ? 1 : 0;
}
