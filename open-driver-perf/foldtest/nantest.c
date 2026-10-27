/* nantest: NMin/NMax matrix on the GPU against a CPU reference, to check the pco lowering of NaN-preserving min/max.
 * Ten inputs (NaN, -inf, -1, -0, +0, 0.5, 1, 2, +inf, 1e30), eight results each: NMax(x,0) NMax(0,x) NMin(x,1) NMin(1,x)
 * NMax(x,-3.5) NMax(-3.5,x) NMax(x,y) NMin(x,y) with y = the next input. Build: gcc -O1 -Wall -o nantest nantest.c -lvulkan -lm
 * Shader: spirv-as --target-env vulkan1.1 nantest.spvasm -o nantest.spv */
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)
static VkPhysicalDevice pd; static VkDevice dev;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
   exit(2);
}
static void *mkbuf(VkBuffer *b, VkDeviceSize sz)
{
   VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
   CK(vkCreateBuffer(dev, &ci, NULL, b));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m)); CK(vkBindBufferMemory(dev, *b, m, 0));
   void *p; CK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p)); return p;
}
static float nmax(float a, float b) { if (isnan(a)) return b; if (isnan(b)) return a; return a > b ? a : b; }
static float nmin(float a, float b) { if (isnan(a)) return b; if (isnan(b)) return a; return a < b ? a : b; }
static int same(float got, float ref) { if (isnan(ref)) return isnan(got); if (isnan(got)) return 0; return got == ref; }
int main(void)
{
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkQueueFamilyProperties qp[8]; uint32_t qn = 8, qf; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
   for (qf = 0; qf < qn; qf++) if (qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
   float prio = 1; VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
   CK(vkCreateDevice(pd, &dci, NULL, &dev)); VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
   VkBuffer bin, bout, bout2; float *in = mkbuf(&bin, 256), *out = mkbuf(&bout, 4096); mkbuf(&bout2, 4096);
   const float v[10] = { NAN, -INFINITY, -1, -0.0f, 0.0f, 0.5f, 1, 2, INFINITY, 1e30f };
   memcpy(in, v, sizeof v); memset(out, 0xAA, 4096);
   FILE *f = fopen("nantest.spv", "rb"); if (!f) { perror("nantest.spv"); return 2; }
   fseek(f, 0, SEEK_END); size_t sn = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *sp = malloc(sn); if (fread(sp, 1, sn, f) != sn) return 2; fclose(f);
   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sn, .pCode = sp }; VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   VkDescriptorSetLayoutBinding bd[3] = { { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
                                          { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
                                          { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bd }; VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl }; VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
   VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }; VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }; VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
   VkDescriptorBufferInfo bi[3] = { { bin, 0, VK_WHOLE_SIZE }, { bout, 0, VK_WHOLE_SIZE }, { bout2, 0, VK_WHOLE_SIZE } };
   VkWriteDescriptorSet w[3] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
                                 { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] } };
   w[2] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[2] };
   vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &cbi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); vkCmdDispatch(cb, 10, 1, 1);
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
   static const char *names[13] = { "NMax(x,0)", "NMax(0,x)", "NMin(x,1)", "NMin(1,x)", "NMax(x,-3.5)", "NMax(-3.5,x)", "NMax(x,y)", "NMin(x,y)", "NMax(NMin(x,1),0)", "NMin(NMax(x,-1),0)", "NClamp(x,0,1)", "NMax(x,NaN)", "NMin(NaN,x)" };
   int fails = 0;
   for (int i = 0; i < 10; i++) {
      float x = v[i], y = v[(i + 1) % 10];
      float ref[13] = { nmax(x, 0), nmax(0, x), nmin(x, 1), nmin(1, x), nmax(x, -3.5f), nmax(-3.5f, x), nmax(x, y), nmin(x, y), nmax(nmin(x, 1), 0), nmin(nmax(x, -1), 0), nmin(nmax(x, 0), 1), nmax(x, NAN), nmin(NAN, x) };
      for (int k = 0; k < 13; k++)
         if (!same(out[i * 16 + k], ref[k])) { fails++; printf("FAIL %-18s x=%g y=%g got %g want %g\n", names[k], x, y, out[i * 16 + k], ref[k]); }
   }
   printf("%s: %d mismatches of 130\n", fails ? "FAILED" : "ALL PASSED", fails);
   return fails ? 1 : 0;
}
