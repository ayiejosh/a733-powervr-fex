/* foldtest: regression test for pvr "hold clear-only dynamic render passes".
 * Each scenario starts with stale contents (A green, B blue) and checks the
 * final pixel colours. Build: see build line in the WORKLOG. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define W 64
#define H 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

struct img { VkImage im; VkDeviceMemory mem; VkImageView view; };

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static uint32_t qf;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkBuffer rb;
static VkDeviceMemory rbm;
static uint8_t *rbp;
static struct img A, B;
static VkSampler samp;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool dpool;
static VkPipelineLayout pl;
static VkPipeline gpipe;
static VkRenderPass legacy_rp;
static VkFramebuffer legacy_fb;
static VkQueryPool qpool;
static int g_query;
static int failures;
static VkPipeline cpipe;
static PFN_vkCmdSetColorWriteEnableEXT pSetCWE;
static VkPipeline tpipe;
static VkPipelineLayout pl3;
static VkDescriptorSetLayout dsl3;
static struct img WH;
static VkDescriptorSetLayout dsl4, dsl5;
static VkPipelineLayout pl4, pl5;
static VkPipeline vpipe, kpipe, upipe;
static VkBuffer obuf, ubuf;
static VkDeviceMemory obufm, ubufm;
static uint8_t *obp, *ubp;
static int only = -1, run_idx;

static const float RED[4] = { 1, 0, 0, 1 }, GREEN[4] = { 0, 1, 0, 1 }, BLUE[4] = { 0, 0, 1, 1 }, YELLOW[4] = { 1, 1, 0, 1 };

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(2);
}

static void make_img(struct img *i)
{
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   CK(vkCreateImage(dev, &ci, NULL, &i->im));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, i->im, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   CK(vkAllocateMemory(dev, &ai, NULL, &i->mem));
   CK(vkBindImageMemory(dev, i->im, i->mem, 0));
   VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->im, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   CK(vkCreateImageView(dev, &vi, NULL, &i->view));
}

static void cmd_begin(void)
{
   CK(vkResetCommandBuffer(cb, 0));
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   CK(vkBeginCommandBuffer(cb, &bi));
}

static void cmd_submit(void)
{
   CK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(q));
}

static void bar_full(void)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* What zink emits between passes: attachment write -> fragment shader read. */
static void bar_frag(void)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void dyn_pass(struct img *t, VkAttachmentLoadOp lo, VkAttachmentStoreOp so, const float c[4], struct img *sample)
{
   VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = lo, .storeOp = so };
   if (c)
      memcpy(ca.clearValue.color.float32, c, 16);
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cb, &ri);
   if (g_query && !sample) {
      vkCmdBeginQuery(cb, qpool, 0, 0);
      vkCmdEndQuery(cb, qpool, 0);
   }
   if (sample) {
      VkDescriptorSet ds;
      VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                         .descriptorSetCount = 1, .pSetLayouts = &dsl };
      CK(vkAllocateDescriptorSets(dev, &ai, &ds));
      VkDescriptorImageInfo ii = { .sampler = samp, .imageView = sample->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
      VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
      vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
      VkViewport vp = { 0, 0, W, H, 0, 1 };
      VkRect2D sc = { { 0, 0 }, { W, H } };
      vkCmdSetViewport(cb, 0, 1, &vp);
      vkCmdSetScissor(cb, 0, 1, &sc);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
      if (g_query)
         vkCmdBeginQuery(cb, qpool, 0, 0);
      vkCmdDraw(cb, 3, 1, 0, 0);
      if (g_query)
         vkCmdEndQuery(cb, qpool, 0);
   }
   vkCmdEndRendering(cb);
}

static void legacy_pass(const float c[4])
{
   VkClearValue cv; memset(&cv, 0, sizeof(cv));
   memcpy(cv.color.float32, c, 16);
   VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = legacy_rp, .framebuffer = legacy_fb,
                                .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(cb);
}

static void copy_both(void)
{
   bar_full();
   VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
   vkCmdCopyImageToBuffer(cb, A.im, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &r);
   r.bufferOffset = W * H * 4;
   vkCmdCopyImageToBuffer(cb, B.im, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &r);
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void setup_stale(void)
{
   cmd_begin();
   VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkClearColorValue g, b;
   memcpy(g.float32, GREEN, 16);
   memcpy(b.float32, BLUE, 16);
   vkCmdClearColorImage(cb, A.im, VK_IMAGE_LAYOUT_GENERAL, &g, 1, &rg);
   vkCmdClearColorImage(cb, B.im, VK_IMAGE_LAYOUT_GENERAL, &b, 1, &rg);
   cmd_submit();
}

static int px_ok(const uint8_t *p, const float e[4])
{
   for (int i = 0; i < 4; i++)
      if (abs((int)p[i] - (int)(e[i] * 255.0f + 0.5f)) > 2)
         return 0;
   return 1;
}

static void check(const char *name, const float ea[4], const float eb[4])
{
   const uint8_t *pa = rbp + (32 * W + 32) * 4, *pb = rbp + W * H * 4 + (32 * W + 32) * 4;
   int ok = px_ok(pa, ea) && px_ok(pb, eb);
   printf("%s %-26s A=%u,%u,%u,%u B=%u,%u,%u,%u\n", ok ? "PASS" : "FAIL", name, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]);
   if (!ok)
      failures++;
}

static void run(const char *name, void (*scenario)(void), const float ea[4], const float eb[4], int copy_in_scenario)
{
   if (only >= 0 && run_idx++ != only)
      return;
   memset(rbp, 0xAA, W * H * 8);
   setup_stale();
   cmd_begin();
   scenario();
   if (copy_in_scenario) {
      copy_both();
      cmd_submit();
   } else {
      cmd_submit();
      cmd_begin();
      copy_both();
      cmd_submit();
   }
   check(name, ea, eb);
}

static void s_baseline(void) { dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL); }
static void s_copy_same_cb(void) { dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL); }
static void s_legacy(void)
{
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   legacy_pass(YELLOW);
}
static void s_query(void)
{
   vkCmdResetQueryPool(cb, qpool, 0, 1);
   g_query = 1;
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   g_query = 0;
   dyn_pass(&B, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, YELLOW, NULL);
}
static void s_store_none(void)
{
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_NONE, NULL, NULL);
}
static void s_consume(void)
{
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, NULL, NULL);
}
static void s_read(void)
{
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   bar_frag();
   dyn_pass(&B, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, YELLOW, &A);
}
/* the zink shape: held clear, unrelated pass, then the pass that loads it */
static void s_zink_shape(void)
{
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   bar_frag();
   dyn_pass(&B, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, YELLOW, NULL);
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, NULL, NULL);
}

static void check_query(const char *name, int nonzero)
{
   uint64_t res = 99;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   VkResult r;
   do {
      r = vkGetQueryPoolResults(dev, qpool, 0, 1, sizeof(res), &res, sizeof(res), VK_QUERY_RESULT_64_BIT);
      clock_gettime(CLOCK_MONOTONIC, &t1);
   } while (r == VK_NOT_READY && t1.tv_sec - t0.tv_sec < 3);
   int ok = r == VK_SUCCESS && (nonzero ? res > 0 : res == 0);
   printf("%s %-26s result=%d samples=%llu\n", ok ? "PASS" : "FAIL", name, r, (unsigned long long)res);
   if (!ok)
      failures++;
}

static void s_query_read(void)
{
   vkCmdResetQueryPool(cb, qpool, 0, 1);
   dyn_pass(&A, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED, NULL);
   bar_frag();
   g_query = 1;
   dyn_pass(&B, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, YELLOW, &A);
   g_query = 0;
}

static void cwe_draw(int right, int enable)
{
   VkViewport vp = { 0, 0, W, H, 0, 1 };
   VkRect2D sc = { { right ? W / 2 : 0, 0 }, { W / 2, H } };
   VkBool32 en = enable;
   vkCmdSetViewport(cb, 0, 1, &vp);
   vkCmdSetScissor(cb, 0, 1, &sc);
   pSetCWE(cb, 1, &en);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
   vkCmdDraw(cb, 3, 1, 0, 0);
}

static void cwe_pass(const int seq[][2], int n)
{
   VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = B.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   memcpy(ca.clearValue.color.float32, BLUE, 16);
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cb, &ri);
   for (int i = 0; i < n; i++)
      cwe_draw(seq[i][0], seq[i][1]);
   vkCmdEndRendering(cb);
}

static void s_cwe_partial(void) { static const int s[][2] = { { 0, 1 }, { 1, 0 } }; cwe_pass(s, 2); }
static void s_cwe_flip_back(void) { static const int s[][2] = { { 0, 1 }, { 1, 0 }, { 1, 1 } }; cwe_pass(s, 3); }
static void s_cwe_flip_first(void) { static const int s[][2] = { { 0, 0 }, { 1, 1 }, { 0, 1 } }; cwe_pass(s, 3); }
static void s_cwe_all_off(void) { static const int s[][2] = { { 0, 0 }, { 1, 0 } }; cwe_pass(s, 2); }

static void run2(const char *name, void (*scenario)(void), const float el[4], const float er[4])
{
   if (only >= 0 && run_idx++ != only)
      return;
   memset(rbp, 0xAA, W * H * 8);
   setup_stale();
   cmd_begin();
   scenario();
   cmd_submit();
   cmd_begin();
   copy_both();
   cmd_submit();
   const uint8_t *bb = rbp + W * H * 4;
   const uint8_t *pl = bb + (32 * W + 16) * 4, *pr = bb + (32 * W + 48) * 4;
   int ok = px_ok(pl, el) && px_ok(pr, er);
   printf("%s %-26s L=%u,%u,%u,%u R=%u,%u,%u,%u\n", ok ? "PASS" : "FAIL", name, pl[0], pl[1], pl[2], pl[3], pr[0], pr[1], pr[2], pr[3]);
   if (!ok)
      failures++;
}

static const float C011[4] = { 0, 1, 1, 1 }, C111[4] = { 1, 1, 1, 1 }, C110[4] = { 1, 1, 0, 1 };

/* nul[i] = descriptor i of the set (a, b[0], b[1]) gets a null image view; the others get the white image. */
static VkDescriptorSet tex_set(const int nul[3])
{
   VkDescriptorSet ds;
   VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl3 };
   CK(vkAllocateDescriptorSets(dev, &ai, &ds));
   VkDescriptorImageInfo ii[3];
   for (int i = 0; i < 3; i++)
      ii[i] = (VkDescriptorImageInfo){ .sampler = samp, .imageView = nul[i] ? VK_NULL_HANDLE : WH.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkWriteDescriptorSet w[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1] } };
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
   return ds;
}

/* One pipeline, one pass, three draws (a third of the target each) that only differ in the bound set (which of the
 * three texture descriptors are null) and in the colour write enable. */
static void tex_pass(const int nulls[3][3], const int cwe[3])
{
   VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = B.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   memcpy(ca.clearValue.color.float32, BLUE, 16);
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cb, &ri);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tpipe);
   for (int i = 0; i < 3; i++) {
      VkDescriptorSet ds = tex_set(nulls[i]);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl3, 0, 1, &ds, 0, NULL);
      VkViewport vp = { 0, 0, W, H, 0, 1 };
      VkRect2D sc = { { i * 21, 0 }, { i == 2 ? W - 42 : 21, H } };
      VkBool32 en = cwe[i];
      vkCmdSetViewport(cb, 0, 1, &vp);
      vkCmdSetScissor(cb, 0, 1, &sc);
      pSetCWE(cb, 1, &en);
      vkCmdDraw(cb, 3, 1, 0, 0);
   }
   vkCmdEndRendering(cb);
}

static const int TEX_NULLS[3][3] = { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 } };
static void s_tex_flip(void) { static const int c[3] = { 1, 1, 1 }; tex_pass(TEX_NULLS, c); }
static void s_tex_cwe(void) { static const int c[3] = { 1, 0, 1 }; tex_pass(TEX_NULLS, c); }

static void run3(const char *name, void (*scenario)(void), const float e0[4], const float e1[4], const float e2[4])
{
   if (only >= 0 && run_idx++ != only)
      return;
   memset(rbp, 0xAA, W * H * 8);
   setup_stale();
   cmd_begin();
   scenario();
   cmd_submit();
   cmd_begin();
   copy_both();
   cmd_submit();
   const uint8_t *bb = rbp + W * H * 4;
   const uint8_t *p0 = bb + (32 * W + 10) * 4, *p1 = bb + (32 * W + 31) * 4, *p2 = bb + (32 * W + 53) * 4;
   int ok = px_ok(p0, e0) && px_ok(p1, e1) && px_ok(p2, e2);
   printf("%s %-26s %u,%u,%u | %u,%u,%u | %u,%u,%u\n", ok ? "PASS" : "FAIL", name, p0[0], p0[1], p0[2], p1[0], p1[1], p1[2], p2[0], p2[1], p2[2]);
   if (!ok)
      failures++;
}

static void make_hbuf(VkBuffer *b, VkDeviceMemory *m, uint8_t **p, VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   CK(vkCreateBuffer(dev, &ci, NULL, b));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   CK(vkAllocateMemory(dev, &ai, NULL, m));
   CK(vkBindBufferMemory(dev, *b, *m, 0));
   CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, (void **)p));
}

static VkDescriptorSet alloc_set(VkDescriptorSetLayout l)
{
   VkDescriptorSet ds;
   VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                      .descriptorSetCount = 1, .pSetLayouts = &l };
   CK(vkAllocateDescriptorSets(dev, &ai, &ds));
   return ds;
}

/* a, b[0], b[1] as in tex_set(); the storage buffer binding points at slot*256 of obuf */
static VkDescriptorSet tex_set4(const int nul[3], int slot)
{
   VkDescriptorSet ds = alloc_set(dsl4);
   VkDescriptorImageInfo ii[3];
   for (int i = 0; i < 3; i++)
      ii[i] = (VkDescriptorImageInfo){ .sampler = samp, .imageView = nul[i] ? VK_NULL_HANDLE : WH.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkDescriptorBufferInfo bi = { .buffer = obuf, .offset = slot * 256, .range = 16 };
   VkWriteDescriptorSet w[3] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi } };
   vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
   return ds;
}

/* one texture and one uniform buffer, either of them null */
static VkDescriptorSet tex_set5(int tex_null, int ubo_null)
{
   VkDescriptorSet ds = alloc_set(dsl5);
   VkDescriptorImageInfo ii = { .sampler = samp, .imageView = tex_null ? VK_NULL_HANDLE : WH.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkDescriptorBufferInfo bi = { .buffer = ubo_null ? VK_NULL_HANDLE : ubuf, .offset = 0, .range = 16 };
   VkWriteDescriptorSet w[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi } };
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
   return ds;
}

/* one pass, one pipeline, three draws (a third of the target each) that only rebind the set */
static void three_draws(VkPipeline pipe, VkPipelineLayout lay, VkDescriptorSet ds[3])
{
   VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = B.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   memcpy(ca.clearValue.color.float32, BLUE, 16);
   VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } },
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cb, &ri);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   for (int i = 0; i < 3; i++) {
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &ds[i], 0, NULL);
      VkViewport vp = { 0, 0, W, H, 0, 1 };
      VkRect2D sc = { { i * 21, 0 }, { i == 2 ? W - 42 : 21, H } };
      VkBool32 en = 1;
      vkCmdSetViewport(cb, 0, 1, &vp);
      vkCmdSetScissor(cb, 0, 1, &sc);
      pSetCWE(cb, 1, &en);
      vkCmdDraw(cb, 3, 1, 0, 0);
   }
   vkCmdEndRendering(cb);
}

static void s_vs_tex(void)
{
   VkDescriptorSet ds[3];
   for (int i = 0; i < 3; i++)
      ds[i] = tex_set4(TEX_NULLS[i], i);
   three_draws(vpipe, pl4, ds);
}

static void s_ubo_tex(void)
{
   VkDescriptorSet ds[3] = { tex_set5(1, 0), tex_set5(0, 0), tex_set5(0, 1) };
   three_draws(upipe, pl5, ds);
}

static void s_comp(void)
{
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kpipe);
   for (int i = 0; i < 3; i++) {
      VkDescriptorSet ds = tex_set4(TEX_NULLS[i], i);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl4, 0, 1, &ds, 0, NULL);
      vkCmdDispatch(cb, 1, 1, 1);
   }
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* the compute dispatches write one vec4 each at obuf + i*256 */
static void runc(const char *name, void (*scenario)(void), const float e0[4], const float e1[4], const float e2[4])
{
   if (only >= 0 && run_idx++ != only)
      return;
   memset(obp, 0xAA, 1024);
   cmd_begin();
   scenario();
   cmd_submit();
   const float *e[3] = { e0, e1, e2 };
   int ok = 1;
   for (int i = 0; i < 3; i++) {
      const float *f = (const float *)(obp + i * 256);
      for (int c = 0; c < 4; c++)
         if (f[c] - e[i][c] > 0.01f || e[i][c] - f[c] > 0.01f)
            ok = 0;
   }
   const float *f0 = (const float *)obp, *f1 = (const float *)(obp + 256), *f2 = (const float *)(obp + 512);
   printf("%s %-26s %.0f,%.0f,%.0f | %.0f,%.0f,%.0f | %.0f,%.0f,%.0f\n", ok ? "PASS" : "FAIL", name,
          f0[0], f0[1], f0[2], f1[0], f1[1], f1[2], f2[0], f2[1], f2[2]);
   if (!ok)
      failures++;
}

static uint32_t *load_spv(const char *path, size_t *n)
{
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(2); }
   fseek(f, 0, SEEK_END);
   *n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *d = malloc(*n);
   if (fread(d, 1, *n, f) != *n) exit(2);
   fclose(f);
   return d;
}

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   if (getenv("FOLD_ONLY"))
      only = atoi(getenv("FOLD_ONLY"));
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   CK(vkEnumeratePhysicalDevices(inst, &n, &pd) == VK_INCOMPLETE ? VK_SUCCESS : VK_SUCCESS);
   VkQueueFamilyProperties qp[8];
   uint32_t qn = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
   for (qf = 0; qf < qn; qf++)
      if (qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         break;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
   VkPhysicalDeviceColorWriteEnableFeaturesEXT fcwe = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, .colorWriteEnable = VK_TRUE };
   VkPhysicalDeviceRobustness2FeaturesEXT fr2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .nullDescriptor = VK_TRUE };
   fcwe.pNext = &fr2;
   f13.pNext = &fcwe;
   const char *dexts[] = { "VK_EXT_color_write_enable", "VK_EXT_robustness2" };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq,
                              .enabledExtensionCount = 2, .ppEnabledExtensionNames = dexts };
   CK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, qf, 0, &q);
   pSetCWE = (PFN_vkCmdSetColorWriteEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetColorWriteEnableEXT");
   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
   CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   CK(vkAllocateCommandBuffers(dev, &cai, &cb));

   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
   CK(vkCreateBuffer(dev, &bci, NULL, &rb));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, rb, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   CK(vkAllocateMemory(dev, &mai, NULL, &rbm));
   CK(vkBindBufferMemory(dev, rb, rbm, 0));
   CK(vkMapMemory(dev, rbm, 0, VK_WHOLE_SIZE, 0, (void **)&rbp));

   make_img(&A);
   make_img(&B);
   cmd_begin();
   VkImageMemoryBarrier ib[2];
   struct img *im[2] = { &A, &B };
   for (int i = 0; i < 2; i++)
      ib[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = im[i]->im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, ib);
   cmd_submit();

   make_img(&WH);
   cmd_begin();
   VkImageMemoryBarrier wb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = WH.im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &wb);
   VkClearColorValue white = { .float32 = { 1, 1, 1, 1 } };
   VkImageSubresourceRange wrg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   vkCmdClearColorImage(cb, WH.im, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &wrg);
   cmd_submit();

   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
   CK(vkCreateSampler(dev, &sci, NULL, &samp));
   VkDescriptorSetLayoutBinding bnd = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bnd };
   CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize dps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 } };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64, .poolSizeCount = 3, .pPoolSizes = dps };
   CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));

   size_t vn, fn;
   uint32_t *vs = load_spv("fold.vert.spv", &vn), *fs = load_spv("fold.frag.spv", &fn);
   VkShaderModule vm, fm;
   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vn, .pCode = vs };
   CK(vkCreateShaderModule(dev, &smi, NULL, &vm));
   smi.codeSize = fn; smi.pCode = fs;
   CK(vkCreateShaderModule(dev, &smi, NULL, &fm));
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
   VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkPipelineRenderingCreateInfo pri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pri, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                        .pMultisampleState = &ms, .pColorBlendState = &cbs, .pDynamicState = &dsi, .layout = pl };
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &gpipe));

   size_t cfn;
   uint32_t *cfs = load_spv("fold2.frag.spv", &cfn);
   VkShaderModule cfm;
   VkShaderModuleCreateInfo csmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = cfn, .pCode = cfs };
   CK(vkCreateShaderModule(dev, &csmi, NULL, &cfm));
   VkPipelineShaderStageCreateInfo cst[2] = { st[0], st[1] };
   cst[1].module = cfm;
   VkPipelineLayoutCreateInfo pl0i = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl0;
   CK(vkCreatePipelineLayout(dev, &pl0i, NULL, &pl0));
   VkDynamicState cdyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT };
   VkPipelineDynamicStateCreateInfo cdsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = cdyn };
   VkGraphicsPipelineCreateInfo cgpi = gpi;
   cgpi.pStages = cst;
   cgpi.pDynamicState = &cdsi;
   cgpi.layout = pl0;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cgpi, NULL, &cpipe));

   size_t tfn;
   uint32_t *tfs = load_spv("tex3.frag.spv", &tfn);
   VkShaderModule tfm;
   VkShaderModuleCreateInfo tsmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = tfn, .pCode = tfs };
   CK(vkCreateShaderModule(dev, &tsmi, NULL, &tfm));
   VkDescriptorSetLayoutBinding tb[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
   VkDescriptorSetLayoutCreateInfo tdli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = tb };
   CK(vkCreateDescriptorSetLayout(dev, &tdli, NULL, &dsl3));
   VkPipelineLayoutCreateInfo pl3i = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl3 };
   CK(vkCreatePipelineLayout(dev, &pl3i, NULL, &pl3));
   VkPipelineShaderStageCreateInfo tst[2] = { st[0], st[1] };
   tst[1].module = tfm;
   VkGraphicsPipelineCreateInfo tgpi = cgpi;
   tgpi.pStages = tst;
   tgpi.layout = pl3;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &tgpi, NULL, &tpipe));

   /* second layout, all stages: sampler a, sampler array b[2], storage buffer (output of the compute test) */
   VkDescriptorSetLayoutBinding t4[3] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL } };
   VkDescriptorSetLayoutCreateInfo d4i = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = t4 };
   CK(vkCreateDescriptorSetLayout(dev, &d4i, NULL, &dsl4));
   VkPipelineLayoutCreateInfo pl4i = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl4 };
   CK(vkCreatePipelineLayout(dev, &pl4i, NULL, &pl4));
   size_t x_vn, x_fn, x_cn, x_un;
   uint32_t *x_vs = load_spv("tex3v.vert.spv", &x_vn), *x_fs = load_spv("flatcol.frag.spv", &x_fn);
   uint32_t *x_cs = load_spv("tex3c.comp.spv", &x_cn), *x_us = load_spv("texubo.frag.spv", &x_un);
   VkShaderModule x_vm, x_fm, x_cm, x_um;
   VkShaderModuleCreateInfo x_smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   x_smi.codeSize = x_vn; x_smi.pCode = x_vs; CK(vkCreateShaderModule(dev, &x_smi, NULL, &x_vm));
   x_smi.codeSize = x_fn; x_smi.pCode = x_fs; CK(vkCreateShaderModule(dev, &x_smi, NULL, &x_fm));
   x_smi.codeSize = x_cn; x_smi.pCode = x_cs; CK(vkCreateShaderModule(dev, &x_smi, NULL, &x_cm));
   x_smi.codeSize = x_un; x_smi.pCode = x_us; CK(vkCreateShaderModule(dev, &x_smi, NULL, &x_um));
   /* vertex shader samples the textures */
   VkPipelineShaderStageCreateInfo x_vst[2] = { st[0], st[1] };
   x_vst[0].module = x_vm; x_vst[1].module = x_fm;
   VkGraphicsPipelineCreateInfo x_vgpi = cgpi;
   x_vgpi.pStages = x_vst; x_vgpi.layout = pl4;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &x_vgpi, NULL, &vpipe));
   /* compute shader samples the textures and stores the result */
   VkComputePipelineCreateInfo x_kci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl4,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = x_cm, .pName = "main" } };
   CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &x_kci, NULL, &kpipe));
   /* fragment shader: one texture and one uniform buffer */
   VkDescriptorSetLayoutBinding t5[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
   VkDescriptorSetLayoutCreateInfo d5i = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = t5 };
   CK(vkCreateDescriptorSetLayout(dev, &d5i, NULL, &dsl5));
   VkPipelineLayoutCreateInfo pl5i = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl5 };
   CK(vkCreatePipelineLayout(dev, &pl5i, NULL, &pl5));
   VkPipelineShaderStageCreateInfo x_ust[2] = { st[0], st[1] };
   x_ust[1].module = x_um;
   VkGraphicsPipelineCreateInfo x_ugpi = cgpi;
   x_ugpi.pStages = x_ust; x_ugpi.layout = pl5;
   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &x_ugpi, NULL, &upipe));
   make_hbuf(&obuf, &obufm, &obp, 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   make_hbuf(&ubuf, &ubufm, &ubp, 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   memset(ubp, 0, 1024);
   ((float *)ubp)[1] = 1.0f;  /* uniform colour (0,1,0,0) */
   ((float *)ubp)[3] = 1.0f;

   VkAttachmentDescription ad = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_GENERAL };
   VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd };
   CK(vkCreateRenderPass(dev, &rpi, NULL, &legacy_rp));
   VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = legacy_rp, .attachmentCount = 1, .pAttachments = &B.view, .width = W, .height = H, .layers = 1 };
   CK(vkCreateFramebuffer(dev, &fbi, NULL, &legacy_fb));
   VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1 };
   CK(vkCreateQueryPool(dev, &qpi, NULL, &qpool));

   run("baseline (end of cmdbuf)", s_baseline, RED, BLUE, 0);
   run("copy in same cmdbuf", s_copy_same_cb, RED, BLUE, 1);
   run("legacy pass after held clear", s_legacy, RED, YELLOW, 0);
   run("occlusion query in clear pass", s_query, RED, YELLOW, 0);
   if (only < 0 || only == 3)
      check_query("query result available", 0);
   run("STORE_OP_NONE after clear", s_store_none, RED, BLUE, 0);
   run("consume (LOAD,STORE)", s_consume, RED, BLUE, 0);
   run("sample the held clear", s_read, RED, RED, 0);
   run("zink shape (A,B,load A)", s_zink_shape, RED, YELLOW, 0);
   run("query in the reading pass", s_query_read, RED, RED, 0);
   if (only < 0 || only == 8)
      check_query("reading-pass query result", 1);
   run2("CWE partial (L on, R off)", s_cwe_partial, RED, BLUE);
   run2("CWE flip back (on,off,on)", s_cwe_flip_back, RED, RED);
   run2("CWE flip first (off,on,on)", s_cwe_flip_first, RED, RED);
   run2("CWE all off", s_cwe_all_off, BLUE, BLUE);
   run3("null textures flip", s_tex_flip, C011, C111, C110);
   run3("null textures + CWE flip", s_tex_cwe, C011, BLUE, C110);
   run3("VS: null textures flip", s_vs_tex, C011, C111, C110);
   run3("FS tex + UBO null", s_ubo_tex, GREEN, YELLOW, RED);
   runc("CS: null textures flip", s_comp, C011, C111, C110);
   printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
   return failures ? 1 : 0;
}
