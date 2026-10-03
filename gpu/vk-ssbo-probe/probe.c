/* Does this driver actually honour vertexPipelineStoresAndAtomics, or only
 * report it? A vertex shader writes to a storage buffer indexed by gl_VertexID;
 * afterwards the buffer is read back. 100/101/102 means the stores landed.
 *
 * Deliberately minimal: no vertex buffer (gl_VertexID is enough), a 4x4 render
 * target that exists only because Vulkan demands one.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   printf("FAIL %s -> %d\n", #x, (int)r_); return 1; } } while (0)

static unsigned *load_spirv(const char *path, size_t *words)
{
   FILE *f = fopen(path, "rb");
   if (!f) { printf("FAIL cannot open %s\n", path); exit(1); }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   unsigned *buf = malloc(sz);
   if (fread(buf, 1, sz, f) != (size_t)sz) { printf("FAIL short read\n"); exit(1); }
   fclose(f);
   *words = sz / 4;
   return buf;
}

static uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return ~0u;
}

int main(void)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   app.apiVersion = VK_API_VERSION_1_0;
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   ici.pApplicationInfo = &app;
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t npd = 0;
   vkEnumeratePhysicalDevices(inst, &npd, NULL);
   if (!npd) { printf("FAIL no Vulkan device\n"); return 1; }
   VkPhysicalDevice *pds = malloc(npd * sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &npd, pds);
   VkPhysicalDevice pd = pds[0];

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("  device: %s\n", props.deviceName);

   VkPhysicalDeviceFeatures have;
   vkGetPhysicalDeviceFeatures(pd, &have);
   printf("  reports vertexPipelineStoresAndAtomics = %s\n",
          have.vertexPipelineStoresAndAtomics ? "true" : "false");
   if (!have.vertexPipelineStoresAndAtomics) {
      printf("RESULT: feature not reported - nothing to test\n");
      return 0;
   }

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qp = malloc(nq * sizeof(*qp));
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
   uint32_t qi = 0;
   for (uint32_t i = 0; i < nq; i++)
      if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   qci.queueFamilyIndex = qi;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   VkPhysicalDeviceFeatures want = {0};
   want.vertexPipelineStoresAndAtomics = VK_TRUE;
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.pEnabledFeatures = &want;
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, qi, 0, &queue);

   /* storage buffer the vertex shader writes into */
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   bci.size = 256;
   bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements bmr;
   vkGetBufferMemoryRequirements(dev, buf, &bmr);
   VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   bai.allocationSize = bmr.size;
   bai.memoryTypeIndex = find_memory(pd, bmr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory bmem;
   CHECK(vkAllocateMemory(dev, &bai, NULL, &bmem));
   CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
   void *mapped = NULL;
   CHECK(vkMapMemory(dev, bmem, 0, 256, 0, &mapped));
   memset(mapped, 0, 256);

   /* 4x4 colour target */
   VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   imci.imageType = VK_IMAGE_TYPE_2D;
   imci.format = VK_FORMAT_R8G8B8A8_UNORM;
   imci.extent.width = 4;
   imci.extent.height = 4;
   imci.extent.depth = 1;
   imci.mipLevels = 1;
   imci.arrayLayers = 1;
   imci.samples = VK_SAMPLE_COUNT_1_BIT;
   imci.tiling = VK_IMAGE_TILING_OPTIMAL;
   imci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   imci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   VkImage img;
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   VkMemoryRequirements imr;
   vkGetImageMemoryRequirements(dev, img, &imr);
   VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   iai.allocationSize = imr.size;
   iai.memoryTypeIndex = find_memory(pd, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &iai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));

   VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   ivci.image = img;
   ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
   ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
   ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   ivci.subresourceRange.levelCount = 1;
   ivci.subresourceRange.layerCount = 1;
   VkImageView view;
   CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

   VkAttachmentDescription att = {0};
   att.format = VK_FORMAT_R8G8B8A8_UNORM;
   att.samples = VK_SAMPLE_COUNT_1_BIT;
   att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = {0};
   sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments = &ref;
   VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
   rpci.attachmentCount = 1;
   rpci.pAttachments = &att;
   rpci.subpassCount = 1;
   rpci.pSubpasses = &sub;
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

   VkImageView attachments[1] = { view };
   VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
   fbci.renderPass = rp;
   fbci.attachmentCount = 1;
   fbci.pAttachments = attachments;
   fbci.width = 4;
   fbci.height = 4;
   fbci.layers = 1;
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   VkDescriptorSetLayoutBinding binding = {0};
   binding.binding = 0;
   binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   binding.descriptorCount = 1;
   binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
   VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   dslci.bindingCount = 1;
   dslci.pBindings = &binding;
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

   VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   dpci.maxSets = 1;
   dpci.poolSizeCount = 1;
   dpci.pPoolSizes = &psz;
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));

   VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   dsai.descriptorPool = dp;
   dsai.descriptorSetCount = 1;
   dsai.pSetLayouts = &dsl;
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));

   VkDescriptorBufferInfo dbi = { buf, 0, 256 };
   VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
   wds.dstSet = ds;
   wds.dstBinding = 0;
   wds.descriptorCount = 1;
   wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   wds.pBufferInfo = &dbi;
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   size_t vsz = 0, fsz = 0;
   unsigned *vs = load_spirv("probe.vert.spv", &vsz);
   unsigned *fs = load_spirv("probe.frag.spv", &fsz);
   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   smci.codeSize = vsz * 4;
   smci.pCode = vs;
   VkShaderModule vsm;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &vsm));
   smci.codeSize = fsz * 4;
   smci.pCode = fs;
   VkShaderModule fsm;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &fsm));

   VkPipelineShaderStageCreateInfo stages[2] = {
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
   };
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vsm;
   stages[0].pName = "main";
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = fsm;
   stages[1].pName = "main";

   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport vp = { 0, 0, 4, 4, 0, 1 };
   VkRect2D sc = { {0, 0}, {4, 4} };
   VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
   vps.viewportCount = 1;
   vps.pViewports = &vp;
   vps.scissorCount = 1;
   vps.pScissors = &sc;
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   rs.lineWidth = 1.0f;
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
   ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState cba = {0};
   cba.colorWriteMask = 0xF;
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
   cb.attachmentCount = 1;
   cb.pAttachments = &cba;

   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   plci.setLayoutCount = 1;
   plci.pSetLayouts = &dsl;
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

   VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
   gpci.stageCount = 2;
   gpci.pStages = stages;
   gpci.pVertexInputState = &vi;
   gpci.pInputAssemblyState = &ia;
   gpci.pViewportState = &vps;
   gpci.pRasterizationState = &rs;
   gpci.pMultisampleState = &ms;
   gpci.pColorBlendState = &cb;
   gpci.layout = pl;
   gpci.renderPass = rp;
   gpci.subpass = 0;
   VkPipeline pipe;
   VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   if (pr != VK_SUCCESS) { printf("FAIL vkCreateGraphicsPipelines -> %d\n", (int)pr); return 1; }

   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   cpci.queueFamilyIndex = qi;
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   cbai.commandPool = cp;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

   VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cmd, &cbbi));
   VkClearValue clear = {0};
   VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
   rpbi.renderPass = rp;
   rpbi.framebuffer = fb;
   rpbi.renderArea.extent.width = 4;
   rpbi.renderArea.extent.height = 4;
   rpbi.clearValueCount = 1;
   rpbi.pClearValues = &clear;
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   unsigned *vals = (unsigned *)mapped;
   printf("  buffer after draw: %u %u %u (expected 100 101 102)\n", vals[0], vals[1], vals[2]);
   if (vals[0] == 100u && vals[1] == 101u && vals[2] == 102u)
      printf("RESULT: vertex-stage storage-buffer stores WORK\n");
   else if (vals[0] == 0u && vals[1] == 0u && vals[2] == 0u)
      printf("RESULT: stores DROPPED - the feature is reported but not honoured\n");
   else
      printf("RESULT: unexpected contents\n");
   return 0;
}
