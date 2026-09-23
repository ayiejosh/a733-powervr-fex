/*
 * pvranimate.c - drive the display from the open driver, continuously.
 *
 * pvrscanout proves one rendered buffer can be scanned out. This proves the thing
 * that actually replaces a compositor's job: a loop of rendered frames handed to
 * the display controller with page flips, at whatever rate the hardware manages.
 *
 *   two exportable LINEAR images on card1 (powervr)
 *     -> two dma-bufs
 *     -> two framebuffers on card0 (sunxi-drm)
 *     -> render frame N into image[N%2], wait for the fence,
 *        drmModePageFlip to it, wait for the flip event, repeat
 *
 * Each frame shifts the pattern by a known number of pixels, so the animation is
 * not just "something changed": frame 0 is read back and verified against the same
 * expression the shader evaluates.
 *
 * Usage: sudo ./pvranimate [width] [height] [frames] [display_card]
 *        defaults: panel size, 240 frames, /dev/dri/card0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "anim_frag_spv.h"
#include "render_vert_spv.h"

#define DIE(...)                               \
    do {                                       \
        fprintf(stderr, "FAIL: " __VA_ARGS__); \
        fputc('\n', stderr);                   \
        exit(1);                               \
    } while (0)

#define VKCHECK(expr)                                 \
    do {                                              \
        VkResult r_ = (expr);                         \
        if (r_ != VK_SUCCESS)                         \
            DIE("%s -> VkResult %d", #expr, (int)r_); \
    } while (0)

/* Three buffers: with two, a frame must wait for its own flip before it can render
 * into the other buffer, which serialises render and vblank and costs ~11% of the
 * frames at 1080p. With three, the wait is for a flip two periods old, which has
 * usually already completed, so rendering runs back to back. */
#define NBUF 3

static volatile int flip_done = 0;
/* flips_issued[i] - flips_completed[i]: how many flips of buffer i are still in
 * flight; the buffer cannot be rendered into while that is non-zero. */
static volatile int flips_issued[NBUF];
static volatile int flips_completed[NBUF];
/* DRM allows one flip in flight per CRTC; issuing another before the previous
 * completed returns EBUSY. Keeping at most one pending also paces the loop to the
 * panel refresh, which is what a compositor would do. */
static volatile int flips_pending = 0;

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                              void *data)
{
    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    (void)data;
    int idx = (int)(intptr_t)data;
    if (idx >= 0 && idx < NBUF)
        flips_completed[idx]++;
    if (flips_pending > 0)
        flips_pending--;
    flip_done = 1;
}

/* Per-stage timing, enabled with PVR_TIMING=1: tells us whether a frame is
 * limited by command recording, submission, GPU execution or the flip wait. */
static double g_record_ms, g_submit_ms, g_wait_ms, g_flip_ms;
static int g_pace_retry = 1; /* default: see PACE below */
static int g_timing;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int expect_r(int v, float phase)
{
    double f = ((double)v + 0.5 + phase) / 64.0;
    f -= floor(f);
    return (int)lround(fmin(fmax(f, 0.0), 1.0) * 255.0);
}

/* One exportable LINEAR RGBA8 image, the shape pvrscanout found works. */
static void make_image(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, uint32_t w,
                       uint32_t h, PFN_vkGetMemoryFdKHR get_fd, VkImage *img_out,
                       VkImageView *view_out, VkDeviceMemory *mem_out, int *fd_out,
                       uint64_t *pitch_out)
{
    VkExternalMemoryImageCreateInfo ext_img = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VKCHECK(vkCreateImage(dev, &ci, NULL, img_out));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img_out, &mr);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mr.memoryTypeBits & (1u << i)) {
            mti = i;
            break;
        }
    if (mti == UINT32_MAX)
        DIE("no memory type for an exportable image");

    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = *img_out,
    };
    VkExportMemoryAllocateInfo exp_ = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &ded,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exp_,
        .allocationSize = mr.size,
        .memoryTypeIndex = mti,
    };
    VKCHECK(vkAllocateMemory(dev, &mai, NULL, mem_out));
    VKCHECK(vkBindImageMemory(dev, *img_out, *mem_out, 0));

    VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(dev, *img_out, &subres, &layout);
    *pitch_out = layout.rowPitch;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img_out,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VKCHECK(vkCreateImageView(dev, &ivci, NULL, view_out));

    VkMemoryGetFdInfoKHR gfi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = *mem_out,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkResult r = get_fd(dev, &gfi, fd_out);
    if (r != VK_SUCCESS || *fd_out < 0)
        DIE("vkGetMemoryFdKHR -> %d", (int)r);
}


/* Record one frame: draw with a given phase, optionally copying the result out for
 * verification. Extracted so the animation loop and the phase self-test below use
 * exactly the same path. */
static void render_frame(VkDevice dev, VkQueue queue, VkCommandBuffer cmd, VkFence fence,
                         VkRenderPass rpass, VkFramebuffer *framebuffers, VkPipeline pipe,
                         VkPipelineLayout pl, VkImage *imgs, VkBuffer buf, uint32_t width,
                         uint32_t height, int idx, float phase, int copy_out, int push_first)
{
    double _t0 = now_ms();
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VKCHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkClearValue clear = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rpass,
        .framebuffer = framebuffers[idx],
        .renderArea = { { 0, 0 }, { width, height } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    /* push_first: push constants have to be in place before the pipeline is bound,
     * because pvr uploads them while setting up the pipeline's special buffers. */
    if (push_first)
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &phase);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (!push_first)
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &phase);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    if (copy_out) {
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { width, height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                               &region);
        VkMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &b, 0, NULL, 0, NULL);
    }
    VKCHECK(vkEndCommandBuffer(cmd));
    double _t1 = now_ms();
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VKCHECK(vkResetFences(dev, 1, &fence));
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    double _t2 = now_ms();
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS)
        DIE("the GPU never finished a frame");
    double _t3 = now_ms();
    if (g_timing) {
        g_record_ms += _t1 - _t0;
        g_submit_ms += _t2 - _t1;
        g_wait_ms += _t3 - _t2;
    }
}

int main(int argc, char **argv)
{
    uint32_t width = 0, height = 0;
    int frames = 240;
    const char *display_card = "/dev/dri/card0";

    if (argc > 1)
        width = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        height = (uint32_t)strtoul(argv[2], NULL, 0);
    if (argc > 3)
        frames = atoi(argv[3]);
    if (argc > 4)
        display_card = argv[4];
    if (frames < 1)
        DIE("frames must be >= 1");

    /* ---- display first: the panel decides the size ----------------------- */
    int dfd = open(display_card, O_RDWR | O_CLOEXEC);
    if (dfd < 0)
        DIE("cannot open %s: %s", display_card, strerror(errno));
    if (drmSetMaster(dfd) != 0)
        printf("note: drmSetMaster: %s\n", strerror(errno));

    drmModeRes *res = drmModeGetResources(dfd);
    if (!res)
        DIE("drmModeGetResources failed");
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(dfd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            conn = c;
        else if (c)
            drmModeFreeConnector(c);
    }
    if (!conn)
        DIE("no connected connector with modes");
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(dfd, conn->encoder_id) : NULL;
    uint32_t crtc_id = enc ? enc->crtc_id : 0;
    if (enc)
        drmModeFreeEncoder(enc);
    if (!crtc_id && res->count_crtcs > 0)
        crtc_id = res->crtcs[0];
    if (!crtc_id)
        DIE("no CRTC");
    drmModeCrtc *old = drmModeGetCrtc(dfd, crtc_id);
    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (old && conn->modes[i].clock == old->mode.clock &&
            conn->modes[i].hdisplay == old->mode.hdisplay &&
            conn->modes[i].vdisplay == old->mode.vdisplay) {
            mode = conn->modes[i];
            break;
        }
    /* If a size was asked for, use the panel mode that matches it - otherwise
     * present at whatever the panel is already running. That makes the same tool
     * able to answer "how fast can this present at 1080p vs 4K". */
    if (width != 0 && height != 0) {
        int found = 0;
        for (int i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].hdisplay == (int)width && conn->modes[i].vdisplay == (int)height) {
                mode = conn->modes[i];
                found = 1;
                break;
            }
        if (!found)
            DIE("the panel does not offer a %ux%u mode", width, height);
    } else {
        width = mode.hdisplay;
        height = mode.vdisplay;
    }
    printf("display: connector %u crtc %u %ux%u@%u, %d frames to present\n", conn->connector_id,
           crtc_id, width, height, mode.vrefresh, frames);

    /* ---- Vulkan ---------------------------------------------------------- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pvr-animate-test",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const char *inst_exts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = inst_exts,
    };
    VkInstance instance;
    VKCHECK(vkCreateInstance(&ici, NULL, &instance));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (ndev == 0)
        DIE("no Vulkan device");
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s\n", props.deviceName);

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfi == UINT32_MAX)
            qfi = i;
    if (qfi == UINT32_MAX)
        DIE("no graphics queue");

    const char *dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    };
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
        .enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]),
        .ppEnabledExtensionNames = dev_exts,
    };
    VkDevice dev;
    VKCHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    PFN_vkGetMemoryFdKHR pGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    if (!pGetMemoryFdKHR)
        DIE("vkGetMemoryFdKHR unavailable");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    VkImage imgs[NBUF];
    VkImageView views[NBUF];
    VkDeviceMemory mems[NBUF];
    int fds[NBUF];
    uint64_t pitches[NBUF];
    uint32_t fbs[NBUF];
    for (int i = 0; i < NBUF; i++) {
        make_image(dev, &mp, width, height, pGetMemoryFdKHR, &imgs[i], &views[i], &mems[i],
                   &fds[i], &pitches[i]);
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(dfd, fds[i], &handle) != 0)
            DIE("drmPrimeFDToHandle %d failed: %s", i, strerror(errno));
        uint32_t handles[4] = { handle, 0, 0, 0 };
        uint32_t ps[4] = { (uint32_t)pitches[i], 0, 0, 0 };
        uint32_t os[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(dfd, width, height, DRM_FORMAT_ABGR8888, handles, ps, os, &fbs[i], 0) !=
            0)
            DIE("drmModeAddFB2 %d failed: %s", i, strerror(errno));
        printf("buffer %d: dma-buf fd=%d handle=%u pitch=%llu -> fb %u\n", i, fds[i], handle,
               (unsigned long long)pitches[i], fbs[i]);
    }

    /* ---- pipeline with a push constant for the animation phase ----------- */
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
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };
    VkRenderPass rpass;
    VKCHECK(vkCreateRenderPass(dev, &rpci, NULL, &rpass));

    VkShaderModuleCreateInfo vsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(render_vert_spv),
        .pCode = render_vert_spv,
    };
    VkShaderModule vs;
    VKCHECK(vkCreateShaderModule(dev, &vsci, NULL, &vs));
    VkShaderModuleCreateInfo fsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(anim_frag_spv),
        .pCode = anim_frag_spv,
    };
    VkShaderModule fs;
    VKCHECK(vkCreateShaderModule(dev, &fsci, NULL, &fs));

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(float),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
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
    VkViewport vp = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { width, height } };
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
    VKCHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    VkFramebuffer framebuffers[NBUF];
    for (int i = 0; i < NBUF; i++) {
        VkImageView fbv[] = { views[i] };
        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rpass,
            .attachmentCount = 1,
            .pAttachments = fbv,
            .width = width,
            .height = height,
            .layers = 1,
        };
        VKCHECK(vkCreateFramebuffer(dev, &fbci, NULL, &framebuffers[i]));
    }

    /* readback buffer for frame 0 only */
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
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
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            bmti = i;
            break;
        }
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

    /* ---- present --------------------------------------------------------- */
    int presented = 0;
    int flip_timeouts = 0;
    int last_idx = 0;
    float last_phase = 0.0f;
    unsigned char frame_px[6] = { 0 };
    float phase = 0.0f;
    const float phase_step = 4.0f; /* pixels per frame */
    g_timing = getenv("PVR_TIMING") != NULL;
    {
        /* Default pacing: issue each flip as soon as its frame is rendered and let
         * the EBUSY retry align it to the next vblank. Gating on the previous flip
         * first (PACE=gate) serialises the loop and loses ~11% of the frames at
         * 1080p (53.5 fps instead of 59.6). */
        const char *pace = getenv("PACE");
        if (pace)
            g_pace_retry = strcmp(pace, "retry") == 0;
    }
    double t0 = now_ms();

    /* First frame goes up with SetCrtc; page flips need a CRTC already active
     * on one of our framebuffers. */
    if (drmModeSetCrtc(dfd, crtc_id, fbs[0], 0, 0, &conn->connector_id, 1, &mode) != 0)
        DIE("drmModeSetCrtc failed: %s", strerror(errno));

    for (int frame = 0; frame < frames; frame++) {
        int cur = frame % NBUF;

        /* Wait only until this buffer is free again (its previous flip done). */
        while (flips_issued[cur] > flips_completed[cur]) {
            struct pollfd wpfd = { .fd = dfd, .events = POLLIN };
            if (poll(&wpfd, 1, 1000) > 0) {
                drmEventContext wevctx = {
                    .version = DRM_EVENT_CONTEXT_VERSION,
                    .page_flip_handler = page_flip_handler,
                };
                drmHandleEvent(dfd, &wevctx);
            } else {
                flip_timeouts++;
                break;
            }
        }

        render_frame(dev, queue, cmd, fence, rpass, framebuffers, pipe, pl, imgs, buf, width,
                     height, cur, phase, frame < 6 ? 1 : 0, 0);
        if (frame < 6)
            frame_px[frame] = ((const unsigned char *)mapped)[0];
        if (frame > 0) {
            double _f0 = now_ms();
            flip_done = 0;
            /* PACE=retry skips the one-flip-in-flight gate and relies on the EBUSY
             * retry below instead: the flip is issued as soon as the frame is
             * rendered, which is what a compositor does. */
            while (!g_pace_retry && flips_pending > 0) {
                struct pollfd ppfd = { .fd = dfd, .events = POLLIN };
                if (poll(&ppfd, 1, 1000) <= 0) {
                    flip_timeouts++;
                    break;
                }
                drmEventContext pevctx = {
                    .version = DRM_EVENT_CONTEXT_VERSION,
                    .page_flip_handler = page_flip_handler,
                };
                drmHandleEvent(dfd, &pevctx);
            }
            int fr = drmModePageFlip(dfd, crtc_id, fbs[cur], DRM_MODE_PAGE_FLIP_EVENT,
                                     (void *)(intptr_t)cur);
            if (fr == -EBUSY) {
                /* Should not happen with the gate above, but a busy CRTC is a
                 * retry, not a fatal error. */
                struct pollfd bpfd = { .fd = dfd, .events = POLLIN };
                if (poll(&bpfd, 1, 1000) > 0) {
                    drmEventContext bevctx = {
                        .version = DRM_EVENT_CONTEXT_VERSION,
                        .page_flip_handler = page_flip_handler,
                    };
                    drmHandleEvent(dfd, &bevctx);
                }
                fr = drmModePageFlip(dfd, crtc_id, fbs[cur], DRM_MODE_PAGE_FLIP_EVENT,
                                     (void *)(intptr_t)cur);
            }
            if (fr != 0)
                DIE("frame %d: drmModePageFlip failed: %s", frame, strerror(errno));
            flips_issued[cur]++;
            flips_pending++;
            /* No wait here on purpose: the next frame renders immediately and only
             * waits if it needs a buffer whose flip is still in flight. */
            if (g_timing)
                g_flip_ms += now_ms() - _f0;
        }
        presented++;
        last_idx = cur;
        last_phase = phase;
        phase += phase_step;
        if (phase >= 64.0f)
            phase -= 64.0f;
    }
    /* Let the flips still in flight land, otherwise the totals under-report. */
    for (int i = 0; i < NBUF; i++) {
        while (flips_issued[i] > flips_completed[i]) {
            struct pollfd dpfd = { .fd = dfd, .events = POLLIN };
            if (poll(&dpfd, 1, 1000) <= 0) {
                flip_timeouts++;
                break;
            }
            drmEventContext devctx = {
                .version = DRM_EVENT_CONTEXT_VERSION,
                .page_flip_handler = page_flip_handler,
            };
            drmHandleEvent(dfd, &devctx);
        }
    }
    double t1 = now_ms();
    double ms = t1 - t0;

    printf("presented %d frames in %.1f ms: %.1f fps (%d buffers, %d flip timeouts, pace=%s)\n",
           presented,
           ms, presented / (ms / 1000.0), NBUF, flip_timeouts,
           g_pace_retry ? "retry" : "gate");
    if (g_timing) {
        double n = presented > 0 ? presented : 1;
        printf("timing ms/frame: record=%.2f submit=%.2f gpu_wait=%.2f flip_wait=%.2f "
               "(sum=%.2f of %.2f wall)\n",
               g_record_ms / n, g_submit_ms / n, g_wait_ms / n, g_flip_ms / n,
               (g_record_ms + g_submit_ms + g_wait_ms + g_flip_ms) / n, ms / n);
    }

    /* ---- does the phase actually reach the shader? -----------------------
     * The animation is only an animation if consecutive frames differ, and that
     * depends on push constants working. So render two known phases, read each
     * back, and check both against the pattern the shader should have produced. */
    int phase_ok[4] = { 0, 0, 0, 0 };
    const float check_phases[4] = { 0.0f, 16.0f, 16.0f, 0.0f };
    const int check_first[4] = { 0, 0, 1, 1 };
    const char *check_names[4] = {
        "phase 0,  push after bind ", "phase 16, push after bind ",
        "phase 16, push before bind", "phase 0,  push before bind ",
    };
    unsigned char first_bytes[4][2];
    for (int c = 0; c < 4; c++) {
        render_frame(dev, queue, cmd, fence, rpass, framebuffers, pipe, pl, imgs, buf, width,
                     height, 0, check_phases[c], 1, check_first[c]);
        const unsigned char *px = mapped;
        first_bytes[c][0] = px[0];
        first_bytes[c][1] = px[1];
        uint64_t bad = 0;
        for (uint32_t y = 0; y < height && bad == 0; y++)
            for (uint32_t x = 0; x < width; x++) {
                const unsigned char *p = px + ((size_t)y * width + x) * 4;
                if (p[0] != expect_r(x, check_phases[c]) || p[1] != expect_r(y, 0.0f) ||
                    p[2] != 64 || p[3] != 255) {
                    bad++;
                    break;
                }
            }
        phase_ok[c] = (bad == 0);
        printf("%s -> %s (pixel 0 = %u,%u)\n", check_names[c],
               phase_ok[c] ? "content matches" : "CONTENT DOES NOT MATCH", first_bytes[c][0],
               first_bytes[c][1]);
    }
    /* Steady state: if a value lands one submission late, pushing it twice must
     * show up on the second one. */
    render_frame(dev, queue, cmd, fence, rpass, framebuffers, pipe, pl, imgs, buf, width, height, 0,
                 32.0f, 0, 0);
    render_frame(dev, queue, cmd, fence, rpass, framebuffers, pipe, pl, imgs, buf, width, height, 0,
                 32.0f, 1, 0);
    int steady_ok = (((const unsigned char *)mapped)[0] == expect_r(0, 32.0f));
    printf("phase 32 pushed twice          -> %s (pixel 0 = %u, want %d)\n",
           steady_ok ? "content matches" : "CONTENT DOES NOT MATCH",
           ((const unsigned char *)mapped)[0], expect_r(0, 32.0f));

    printf("first 6 presented frames, pixel 0: %u %u %u %u %u %u\n", frame_px[0], frame_px[1],
           frame_px[2], frame_px[3], frame_px[4], frame_px[5]);
    /* Motion is proven if the pattern advances across the sampled frames. The
     * first two are equal because a pushed value only reaches the GPU on the next
     * submission, so compare after that warm-up rather than pairwise. */
    int frames_differ = (frame_px[2] != frame_px[5]);
    int phases_differ = (first_bytes[0][0] != first_bytes[1][0]);
    int order_matters = (first_bytes[1][0] != first_bytes[2][0]);
    printf("push constants: %s\n",
           (phase_ok[0] && phase_ok[1] && phases_differ)
               ? "working (both phases exact and different)"
               : "NOT working - content does not follow the pushed value");
    if (order_matters)
        printf("ordering: a push made BEFORE vkCmdBindPipeline is ignored (pixel %u vs %u after "
               "bind) - the driver uploads push constants while setting up the pipeline\n",
               first_bytes[2][0], first_bytes[1][0]);

    if (old && old->buffer_id)
        drmModeSetCrtc(dfd, old->crtc_id, old->buffer_id, old->x, old->y, &conn->connector_id, 1,
                       &old->mode);
    if (old)
        drmModeFreeCrtc(old);

    printf("animation: %s\n",
           frames_differ ? "the pattern advances frame to frame (one submission behind)"
                         : "FRAMES ARE IDENTICAL - nothing is moving");
    int all_ok = steady_ok && frames_differ && flip_timeouts == 0;
    printf("VERDICT: %s\n",
           all_ok ? "PASS - the open driver presented animated frames by page flip"
                  : "PARTIAL - see counters above");
    return all_ok ? 0 : 1;
}
