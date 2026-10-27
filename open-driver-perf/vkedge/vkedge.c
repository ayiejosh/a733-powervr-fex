/* vkedge - triangle-edge coverage gate.
 *
 * Draws fixed triangles whose vertices sit on a 1/16-pixel grid into a cleared
 * RGBA8 image, reads it back and checks every pixel against an exact CPU
 * reference:
 *   samples=1  coverage = pixel centre inside a triangle; pixels whose centre lies
 *              exactly on an edge are skipped (tie-break rules not modelled).
 *   samples=4  only pixels whose 4 standard sample positions are ALL inside or ALL
 *              outside are checked; partially covered pixels are skipped.
 * A colour outside the triangles (leak, e.g. from a helper invocation) or a
 * missing colour inside (hole) fails the run.
 *
 *   usage: vkedge [size=256] [samples=1|4] [shader=c|d|b]
 *          c = constant colour, d = colour computed through dFdx (forces helpers)
 *          b = c, then a full-screen triangle blended over it (src-alpha) with
 *              edge_b.frag; checks the blended result inside and outside
 * Shaders are loaded from <dir of binary>/edge.vert.spv and edge_<shader>.frag.spv.
 */
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "FAIL %s -> %d\n", #x, r_); exit(1); } } while (0)

/* Triangles in pixel units for a 256x256 target; every coordinate is a multiple of 1/16. */
static const double TRIS[][6] = {
   { 10.3125, 12.0625, 200.5625, 30.1875, 40.4375, 180.8125 },   /* large */
   { 120.6875, 140.1875, 250.9375, 150.3125, 130.0625, 252.4375 }, /* large, other winding */
   { 5.1875, 200.5625, 90.8125, 203.0625, 6.4375, 205.6875 },     /* thin sliver */
   { 220.0625, 5.3125, 222.9375, 5.8125, 221.1875, 9.0625 },      /* tiny, a few pixels */
   { 60.5625, 60.5625, 61.4375, 140.9375, 62.3125, 60.6875 },     /* near-vertical sliver */
   { 150.3125, 20.6875, 190.8125, 20.8125, 170.4375, 21.9375 },   /* near-horizontal sliver */
   { 100.0625, 100.0625, 101.9375, 100.0625, 101.0, 101.9375 },   /* ~2-pixel triangle */
   { 30.8125, 230.3125, 31.1875, 250.6875, 70.5625, 249.4375 },   /* edge-heavy */
};
#define NTRI (sizeof(TRIS) / sizeof(TRIS[0]))

static const double SPOS4[4][2] = { { 0.375, 0.125 }, { 0.875, 0.375 }, { 0.125, 0.625 }, { 0.625, 0.875 } };

static int inside(const double *t, double px, double py, int *tie)
{
   double e[3];
   for (int i = 0; i < 3; i++) {
      const double ax = t[2 * i], ay = t[2 * i + 1];
      const double bx = t[2 * ((i + 1) % 3)], by = t[2 * ((i + 1) % 3) + 1];
      e[i] = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
      if (e[i] == 0.0)
         *tie = 1;
   }
   return (e[0] > 0 && e[1] > 0 && e[2] > 0) || (e[0] < 0 && e[1] < 0 && e[2] < 0);
}

/* 1 = covered, 0 = not covered, -1 = skip (tie or partial). */
static int expect_cov(double px, double py, int samples, double scale)
{
   int n = 0, tie = 0;
   const int ns = samples == 1 ? 1 : 4;
   for (int s = 0; s < ns; s++) {
      const double sx = (samples == 1 ? 0.5 : SPOS4[s][0]) + px, sy = (samples == 1 ? 0.5 : SPOS4[s][1]) + py;
      int hit = 0;
      for (unsigned i = 0; i < NTRI; i++) {
         double t[6];
         for (int k = 0; k < 6; k++)
            t[k] = TRIS[i][k] * scale;
         hit |= inside(t, sx, sy, &tie);
      }
      n += hit;
   }
   if (tie)
      return -1;
   if (n == 0)
      return 0;
   if (n == ns)
      return 1;
   return -1;
}

static uint32_t *load_spv(const char *dir, const char *name, size_t *sz)
{
   char p[1024];
   snprintf(p, sizeof(p), "%s/%s", dir, name);
   FILE *f = fopen(p, "rb");
   if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", p); exit(1); }
   fseek(f, 0, SEEK_END);
   *sz = (size_t)ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *b = malloc(*sz);
   if (fread(b, 1, *sz, f) != *sz) { fprintf(stderr, "FAIL: read %s\n", p); exit(1); }
   fclose(f);
   return b;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "FAIL: no memory type\n");
   exit(1);
}

static VkDeviceMemory alloc_bind_image(VkDevice dev, VkPhysicalDevice pd, VkImage img)
{
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   VkDeviceMemory m;
   CK(vkAllocateMemory(dev, &ai, NULL, &m));
   CK(vkBindImageMemory(dev, img, m, 0));
   return m;
}

static VkImage make_image(VkDevice dev, uint32_t size, VkSampleCountFlagBits samples, VkImageUsageFlags usage)
{
   VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { size, size, 1 }, .mipLevels = 1,
                            .arrayLayers = 1, .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img;
   CK(vkCreateImage(dev, &ci, NULL, &img));
   return img;
}

static VkImageView make_view(VkDevice dev, VkImage img)
{
   VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView v;
   CK(vkCreateImageView(dev, &ci, NULL, &v));
   return v;
}

int main(int argc, char **argv)
{
   const uint32_t size = argc > 1 ? (uint32_t)atoi(argv[1]) : 256;
   const int samples = argc > 2 ? atoi(argv[2]) : 1;
   const char shader = argc > 3 ? argv[3][0] : 'c';
   if ((samples != 1 && samples != 4) || (shader != 'c' && shader != 'd' && shader != 'b') || size < 64) {
      fprintf(stderr, "usage: %s [size>=64] [samples=1|4] [shader=c|d|b]\n", argv[0]);
      return 2;
   }
   const double scale = size / 256.0;
   char dir[1024];
   snprintf(dir, sizeof(dir), "%s", argv[0]);
   char *slash = strrchr(dir, '/');
   if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");

   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t npd = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &npd, &pd);
   if (!npd) { fprintf(stderr, "FAIL: no device\n"); return 1; }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s | size %u samples %d shader %c | subpixel bits %u | standardSampleLocations %u\n",
          props.deviceName, size, samples, shader, props.limits.subPixelPrecisionBits,
          props.limits.standardSampleLocations);
   if (samples == 4 && !props.limits.standardSampleLocations) {
      printf("RESULT: SKIP - non-standard sample locations\n");
      return 0;
   }

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qf[16];
   if (nq > 16) nq = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t qfi = 0;
   while (qfi < nq && !(qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi++;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev;
   CK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, qfi, 0, &q);

   /* Targets. */
   const VkSampleCountFlagBits sc = samples == 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
   VkImage resolved = make_image(dev, size, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   alloc_bind_image(dev, pd, resolved);
   VkImageView vres = make_view(dev, resolved);
   VkImage ms = VK_NULL_HANDLE;
   VkImageView vms = VK_NULL_HANDLE;
   if (samples > 1) {
      ms = make_image(dev, size, sc, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
      alloc_bind_image(dev, pd, ms);
      vms = make_view(dev, ms);
   }

   /* Render pass: clear to 0, store (or resolve) the colour. */
   VkAttachmentDescription att[2] = {
      { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = sc, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = samples > 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
      { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
   };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference rref = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &cref, .pResolveAttachments = samples > 1 ? &rref : NULL };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = samples > 1 ? 2 : 1, .pAttachments = att, .subpassCount = 1,
                                   .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp;
   CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkImageView fbv[2] = { samples > 1 ? vms : vres, vres };
   VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                   .attachmentCount = samples > 1 ? 2 : 1, .pAttachments = fbv, .width = size,
                                   .height = size, .layers = 1 };
   VkFramebuffer fb;
   CK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

   /* Pipeline. */
   size_t vsz, fsz;
   char fname[32];
   snprintf(fname, sizeof(fname), "edge_%c.frag.spv", shader == 'b' ? 'c' : shader);
   uint32_t *vcode = load_spv(dir, "edge.vert.spv", &vsz), *fcode = load_spv(dir, fname, &fsz);
   VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsz, .pCode = vcode };
   VkShaderModule vsm, fsm;
   CK(vkCreateShaderModule(dev, &smci, NULL, &vsm));
   smci.codeSize = fsz;
   smci.pCode = fcode;
   CK(vkCreateShaderModule(dev, &smci, NULL, &fsm));
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" },
   };
   VkVertexInputBindingDescription vb = { 0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &va };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, (float)size, (float)size, 0, 1 };
   VkRect2D sci = { { 0, 0 }, { size, size } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sci };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo mss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = sc };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba };
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl;
   CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                         .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                         .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &mss,
                                         .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
   VkPipeline pipe;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

   /* Mode b: a second pipeline, same vertex shader, constant (0,1,0,0.5) blended
    * with SRC_ALPHA / ONE_MINUS_SRC_ALPHA (colour and alpha) over the first draw. */
   VkPipeline bpipe = VK_NULL_HANDLE;
   if (shader == 'b') {
      size_t bsz;
      uint32_t *bcode = load_spv(dir, "edge_b.frag.spv", &bsz);
      VkShaderModule bsm;
      smci.codeSize = bsz;
      smci.pCode = bcode;
      CK(vkCreateShaderModule(dev, &smci, NULL, &bsm));
      st[1].module = bsm;
      VkPipelineColorBlendAttachmentState bba = {
         .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
         .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
         .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
         .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf };
      VkPipelineColorBlendStateCreateInfo bcb = cb;
      bcb.pAttachments = &bba;
      VkGraphicsPipelineCreateInfo bgpci = gpci;
      bgpci.pColorBlendState = &bcb;
      CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &bgpci, NULL, &bpipe));
   }

   /* Vertex buffer (NDC; mode b adds a full-screen triangle) and readback buffer, host visible. */
   const VkDeviceSize vbytes = (NTRI + 1) * 3 * 2 * sizeof(float), rbytes = (VkDeviceSize)size * size * 4;
   VkBuffer vbuf, rbuf;
   VkDeviceMemory vmem, rmem;
   for (int i = 0; i < 2; i++) {
      VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = i ? rbytes : vbytes,
                                 .usage = i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
      VkBuffer *b = i ? &rbuf : &vbuf;
      VkDeviceMemory *m = i ? &rmem : &vmem;
      CK(vkCreateBuffer(dev, &bci, NULL, b));
      VkMemoryRequirements mr;
      vkGetBufferMemoryRequirements(dev, *b, &mr);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                  .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits,
                                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
      CK(vkAllocateMemory(dev, &ai, NULL, m));
      CK(vkBindBufferMemory(dev, *b, *m, 0));
   }
   float *vdata;
   CK(vkMapMemory(dev, vmem, 0, vbytes, 0, (void **)&vdata));
   for (unsigned i = 0; i < NTRI; i++)
      for (int k = 0; k < 6; k++)
         vdata[i * 6 + k] = (float)(TRIS[i][k] * scale / size * 2.0 - 1.0);
   const float full[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
   memcpy(vdata + NTRI * 6, full, sizeof(full));
   vkUnmapMemory(dev, vmem);

   /* Record, submit, wait. */
   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool;
   CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CK(vkBeginCommandBuffer(cmd, &bi));
   VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 0 } } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { size, size } }, .clearValueCount = 1, .pClearValues = &clear };
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize zero = 0;
   vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero);
   vkCmdDraw(cmd, NTRI * 3, 1, 0, 0);
   if (bpipe) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bpipe);
      vkCmdDraw(cmd, 3, 1, NTRI * 3, 0);
   }
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { size, size, 1 } };
   vkCmdCopyImageToBuffer(cmd, resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &reg);
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CK(vkEndCommandBuffer(cmd));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(q));

   /* Check every pixel. */
   const uint8_t *px;
   CK(vkMapMemory(dev, rmem, 0, rbytes, 0, (void **)&px));
   long checked = 0, skipped = 0, covered = 0, leaks = 0, holes = 0, wrong = 0;
   for (uint32_t y = 0; y < size; y++) {
      for (uint32_t x = 0; x < size; x++) {
         const int e = expect_cov(x, y, samples, scale);
         if (e < 0) { skipped++; continue; }
         checked++;
         const uint8_t *p = px + ((size_t)y * size + x) * 4;
         int is_bg = !p[0] && !p[1] && !p[2] && !p[3];
         int is_fg = p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255;
         if (bpipe) {
            /* blended over magenta: (0.5,0.5,0.5,0.75); over the clear: (0,0.5,0,0.25); +-1 for rounding */
#define NEAR(v, t) ((v) >= (t) - 1 && (v) <= (t) + 1)
            is_fg = NEAR(p[0], 128) && NEAR(p[1], 128) && NEAR(p[2], 128) && NEAR(p[3], 191);
            is_bg = p[0] == 0 && NEAR(p[1], 128) && p[2] == 0 && NEAR(p[3], 64);
#undef NEAR
         }
         if (e) {
            covered++;
            if (is_bg) { if (holes++ < 5) printf("  hole at (%u,%u)\n", x, y); }
            else if (!is_fg) { if (wrong++ < 5) printf("  wrong colour at (%u,%u): %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]); }
         } else if (!is_bg) {
            if (leaks++ < 5) printf("  leak at (%u,%u): %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]);
         }
      }
   }
   vkUnmapMemory(dev, rmem);
   printf("checked %ld pixels (%ld covered), skipped %ld (edge ties%s)\n", checked, covered, skipped,
          samples > 1 ? " or partial coverage" : "");
   if (leaks || holes || wrong || covered == 0) {
      printf("RESULT: FAIL - %ld leaks, %ld holes, %ld wrong colours\n", leaks, holes, wrong);
      return 1;
   }
   printf("RESULT: PASS - %ld/%ld pixels correct\n", checked, checked);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
