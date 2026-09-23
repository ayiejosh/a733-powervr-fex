/*
 * vkrender.c - does the PowerVR driver RENDER, or only compute?
 *
 * vktest.c proves the compute queue executes. A GPU driver can pass that and
 * still be unable to run a graphics pipeline at all: render passes, tile buffers,
 * MSAA resolve, image layouts and the transfer queue are a separate machinery,
 * and on this board that machinery is exactly what was unproven.
 *
 * So this draws one full-screen triangle into an offscreen 512x512 image with a
 * fragment shader whose output the host can predict exactly, resolves it to
 * TRANSFER_SRC layout, copies it to a host-visible buffer, and compares every
 * pixel. No window, no WSI, no swapchain: offscreen rendering is the part the
 * driver owns.
 *
 *   vkCreateImage -> render pass -> graphics pipeline -> vkCmdDraw(3)
 *   -> vkCmdCopyImageToBuffer -> readback -> per-pixel compare
 *
 * Build: see build.sh   Run: VK_ICD_FILENAMES=<icd.json> ./vkrender [size]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "render_frag_spv.h"
#include "render_vert_spv.h"

#define DIE(...)                               \
    do {                                       \
        fprintf(stderr, "FAIL: " __VA_ARGS__); \
        fputc('\n', stderr);                   \
        exit(1);                               \
    } while (0)

#define VKCHECK(expr)                                          \
    do {                                                       \
        VkResult r_ = (expr);                                  \
        if (r_ != VK_SUCCESS)                                  \
            DIE("%s -> VkResult %d", #expr, (int)r_);           \
    } while (0)

/* Unorm conversion is round(f * 255), so the host can predict stored bytes. */
static int expect_r(int x) { return (int)lroundf(fminf(fmaxf((float)((x + 0.5) / 64.0 - floor((x + 0.5) / 64.0)), 0.0f), 1.0f) * 255.0f); }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    uint32_t size = 512;
    int iters = 1;

    if (argc > 1)
        size = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        iters = atoi(argv[2]);
    if (size < 64 || size > 4096 || iters < 1)
        DIE("bad arguments: size=%u (64..4096) iters=%d", size, iters);

    uint32_t api = VK_API_VERSION_1_0;
    const char *api_env = getenv("VKTEST_API");
    if (api_env) {
        unsigned maj = 1, min = 0;
        if (sscanf(api_env, "%u.%u", &maj, &min) == 2)
            api = VK_MAKE_VERSION(maj, min, 0);
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pvr-vulkan-render-test",
        .apiVersion = api,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    VKCHECK(vkCreateInstance(&ici, NULL, &instance));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (ndev == 0)
        DIE("no Vulkan physical device");
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));

    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s (api %u.%u.%u)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);

    /* A graphics queue is the whole point here; do not silently accept less. */
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfi == UINT32_MAX)
            qfi = i;
    }
    if (qfi == UINT32_MAX) {
        printf("RESULT: FAIL - no graphics queue family (only compute/transfer)\n");
        printf("VERDICT: FAIL\n");
        return 1;
    }
    printf("graphics queue family %u (flags=0x%x, count=%u)\n", qfi, qf[qfi].queueFlags,
           qf[qfi].queueCount);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    /* ---- colour image -------------------------------------------------- */
    VkImageCreateInfo imci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { size, size, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    VKCHECK(vkCreateImage(dev, &imci, NULL, &image));

    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(dev, image, &imr);

    uint32_t imti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((imr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            imti = i;
            break;
        }
    }
    if (imti == UINT32_MAX)
        DIE("no device-local memory type for the colour image");

    VkMemoryAllocateInfo imai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = imr.size,
        .memoryTypeIndex = imti,
    };
    VkDeviceMemory imem;
    VKCHECK(vkAllocateMemory(dev, &imai, NULL, &imem));
    VKCHECK(vkBindImageMemory(dev, image, imem, 0));

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView view;
    VKCHECK(vkCreateImageView(dev, &ivci, NULL, &view));

    /* ---- render pass --------------------------------------------------- */
    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference attref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    VkRenderPass rpass;
    VKCHECK(vkCreateRenderPass(dev, &rpci, NULL, &rpass));

    VkImageView fb_views[] = { view };
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rpass,
        .attachmentCount = 1,
        .pAttachments = fb_views,
        .width = size,
        .height = size,
        .layers = 1,
    };
    VkFramebuffer fb;
    VKCHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

    /* ---- pipeline ------------------------------------------------------ */
    VkShaderModuleCreateInfo vsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(render_vert_spv),
        .pCode = render_vert_spv,
    };
    VkShaderModule vs;
    VKCHECK(vkCreateShaderModule(dev, &vsci, NULL, &vs));

    VkShaderModuleCreateInfo fsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(render_frag_spv),
        .pCode = render_frag_spv,
    };
    VkShaderModule fs;
    VKCHECK(vkCreateShaderModule(dev, &fsci, NULL, &fs));

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VkPipelineLayout pl;
    VKCHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)size, (float)size, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { size, size } };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &vp,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms_state,
        .pColorBlendState = &cb,
        .layout = pl,
        .renderPass = rpass,
        .subpass = 0,
    };
    VkPipeline pipe;
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    if (pr != VK_SUCCESS)
        DIE("vkCreateGraphicsPipelines -> %d (the graphics path is not usable)", (int)pr);
    printf("graphics pipeline created\n");

    /* ---- readback buffer ----------------------------------------------- */
    VkDeviceSize bytes = (VkDeviceSize)size * size * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(dev, buf, &bmr);

    uint32_t bmti = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            bmti = i;
            break;
        }
    }
    if (bmti == UINT32_MAX)
        DIE("no host-visible coherent memory type for readback");
    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bmr.size,
        .memoryTypeIndex = bmti,
    };
    VkDeviceMemory bmem;
    VKCHECK(vkAllocateMemory(dev, &bmai, NULL, &bmem));
    VKCHECK(vkBindBufferMemory(dev, buf, bmem, 0));
    void *mapped = NULL;
    VKCHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &mapped));

    /* ---- commands ------------------------------------------------------ */
    VkCommandPoolCreateInfo cpi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfi,
    };
    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VKCHECK(vkCreateFence(dev, &fci, NULL, &fence));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    printf("rendering %d x %ux%u offscreen frames...\n", iters, size, size);
    double t0 = now_ms();
    for (int it = 0; it < iters; it++) {
        VkCommandBufferBeginInfo cbbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));

        VkClearValue clear = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rpass,
            .framebuffer = fb,
            .renderArea = { { 0, 0 }, { size, size } },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { size, size, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                               &region);
        VKCHECK(vkEndCommandBuffer(cmd));

        VKCHECK(vkResetFences(dev, 1, &fence));
        VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
        VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
        if (w != VK_SUCCESS)
            DIE("vkWaitForFences after %d frame(s) -> %d (GPU never signalled)", it + 1,
                (int)w);
    }
    double t1 = now_ms();

    /* ---- verify -------------------------------------------------------- */
    const unsigned char *px = mapped;
    uint64_t bad = 0;
    uint32_t fx = 0, fy = 0;
    int er = 0, eg = 0, eb = 0, ea = 0, gr = 0, gg = 0, gb = 0, ga = 0;
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const unsigned char *p = px + ((size_t)y * size + x) * 4;
            int wr = expect_r(x), wg = expect_r(y), wb = 64, wa = 255;
            if (p[0] != wr || p[1] != wg || p[2] != wb || p[3] != wa) {
                if (bad == 0) {
                    fx = x; fy = y;
                    er = wr; eg = wg; eb = wb; ea = wa;
                    gr = p[0]; gg = p[1]; gb = p[2]; ga = p[3];
                }
                bad++;
            }
        }
    }

    double ms_total = t1 - t0;
    printf("%d frame(s) in %.3f ms (%.3f ms/frame, %.1f Mpix/s)\n", iters, ms_total,
           ms_total / iters, (double)size * size * iters / (ms_total / 1000.0) / 1e6);
    if (bad) {
        printf("RESULT: FAIL - %llu/%u pixels wrong (first at %u,%u: want %d,%d,%d,%d got %d,%d,%d,%d)\n",
               (unsigned long long)bad, size * size, fx, fy, er, eg, eb, ea, gr, gg, gb, ga);
    } else {
        printf("RESULT: PASS - %u/%u pixels correct\n", size * size, size * size);
    }
    printf("VERDICT: %s\n", bad ? "FAIL" : "PASS");

    return bad ? 1 : 0;
}
