p = "/home/radxa/open-perf/foldtest/foldtest.c"
s = open(p).read()


def rep(old, new):
    global s
    assert s.count(old) == 1, (old[:70], s.count(old))
    s = s.replace(old, new)


rep("static PFN_vkCmdSetColorWriteEnableEXT pSetCWE;\n",
    "static PFN_vkCmdSetColorWriteEnableEXT pSetCWE;\nstatic VkPipeline tpipe;\nstatic VkPipelineLayout pl3;\nstatic VkDescriptorSetLayout dsl3;\nstatic struct img WH;\n")
rep('   f13.pNext = &fcwe;\n   const char *dexts[] = { "VK_EXT_color_write_enable" };\n',
    '   VkPhysicalDeviceRobustness2FeaturesEXT fr2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .nullDescriptor = VK_TRUE };\n   fcwe.pNext = &fr2;\n   f13.pNext = &fcwe;\n   const char *dexts[] = { "VK_EXT_color_write_enable", "VK_EXT_robustness2" };\n')
rep(".enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };", ".enabledExtensionCount = 2, .ppEnabledExtensionNames = dexts };")
rep("{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 }", "{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }")
rep(".maxSets = 32,", ".maxSets = 64,")
rep("   VkSamplerCreateInfo sci = {",
    """   make_img(&WH);
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

   VkSamplerCreateInfo sci = {""")
rep("   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cgpi, NULL, &cpipe));\n",
    """   CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cgpi, NULL, &cpipe));

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
""")
fn = r'''static const float C011[4] = { 0, 1, 1, 1 }, C111[4] = { 1, 1, 1, 1 }, C110[4] = { 1, 1, 0, 1 };

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

'''
k = s.index("static uint32_t *load_spv")
s = s[:k] + fn + s[k:]
rep('   run2("CWE all off", s_cwe_all_off, BLUE, BLUE);\n',
    '   run2("CWE all off", s_cwe_all_off, BLUE, BLUE);\n   run3("null textures flip", s_tex_flip, C011, C111, C110);\n   run3("null textures + CWE flip", s_tex_cwe, C011, BLUE, C110);\n')
open(p, "w").write(s)
print("ok")
