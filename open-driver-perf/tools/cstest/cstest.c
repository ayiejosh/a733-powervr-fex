/* cstest SHADER.spv OUT.bin [N]: run a compute shader (local_size_x 64, set 0: binding 0 = input uint[N*4],
 * binding 1 = output uint[N*16]) over N invocations and write the output buffer to OUT.bin. Run it on two Vulkan
 * drivers (VK_ICD_FILENAMES) and compare the files: lavapipe is the reference. Input i is a fixed pseudo-random
 * pattern so both runs see the same data. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "FAIL %s = %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd;
static uint32_t memtype(uint32_t bits) {
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   exit(2);
}
static void *mkbuf(VkDeviceSize size, VkBuffer *b) {
   VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
   CK(vkCreateBuffer(dev, &bi, NULL, b));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits) };
   VkDeviceMemory m; void *p; CK(vkAllocateMemory(dev, &ai, NULL, &m)); CK(vkBindBufferMemory(dev, *b, m, 0)); CK(vkMapMemory(dev, m, 0, size, 0, &p));
   return p;
}
int main(int argc, char **argv) {
   if (argc < 3) { fprintf(stderr, "usage: cstest shader.spv out.bin [N]\n"); return 2; }
   const uint32_t N = argc > 3 ? atoi(argv[3]) : 4096;
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props); fprintf(stderr, "device: %s\n", props.deviceName);
   float pr = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pr };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   CK(vkCreateDevice(pd, &di, NULL, &dev)); VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
   VkBuffer ib, ob; uint32_t *in = mkbuf(N * 16, &ib), *out = mkbuf(N * 64, &ob);
   uint32_t s = 12345; for (uint32_t i = 0; i < N * 4; i++) { s = s * 1103515245u + 12345u; in[i] = s >> 3; }
   for (uint32_t i = 0; i < N * 4; i += 4) { in[i + 3] = in[i + 3] % 37; } /* small loop counts in lane .w */
   memset(out, 0xcd, N * 64);
   FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
   fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *code = malloc(sz); fread(code, 1, sz, f); fclose(f);
   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
   VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   VkDescriptorSetLayoutBinding bd[2] = { { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
                                          { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd };
   VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
   VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
   VkDescriptorBufferInfo bi0 = { ib, 0, VK_WHOLE_SIZE }, bi1 = { ob, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet w[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi0 },
                                 { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi1 } };
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bgi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, N / 64, 1, 1); CK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
   f = fopen(argv[2], "wb"); fwrite(out, 1, N * 64, f); fclose(f);
   return 0;
}
