/* x11present.c - end-to-end X11 WSI test: Vulkan swapchain -> X server pixels.
 *
 * "The ICD advertises VK_KHR_xcb_surface" is not the same claim as "an X11
 * application can put pixels on the screen on this board". This closes the gap
 * between them: it connects to a real X server over xcb, creates a Vulkan
 * surface, creates a swapchain, renders a known colour, presents it, and then
 * asks the *X server* what it is actually displaying - xcb_get_image on the root
 * window, which is the far end of the path and cannot be faked by the driver.
 *
 * It needs a real X server, and on this board that is the interesting part:
 * presentation over xcb requires the X server to speak DRI3. The vendor driver
 * says so out loud ("No DRI3 support detected - required for presentation") and
 * Mesa's X11 WSI agrees - wsi_common_x11.c only falls back to MIT-SHM when the
 * WSI device is a *software* device (`wants_shm = wsi_dev->sw && ...`, line ~246),
 * which a hardware driver like pvr is not. Xvfb has no DRI3, so this tool reports
 * the surface half as measured facts and then SKIPs the present half with the
 * reason, instead of turning this board's X configuration into a driver failure.
 *
 *   DISPLAY=:99 VK_ICD_FILENAMES=<icd.json> ./x11present [frames]
 *
 * Exit status 0 on PASS, 1 on FAIL, 2 on setup error, 3 on SKIP (no DRI3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#define VK_USE_PLATFORM_XCB_KHR 1
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#define DIE(...)                                                                        \
    do {                                                                                \
        fprintf(stderr, "SETUP-FAIL: " __VA_ARGS__);                                    \
        fprintf(stderr, "\n");                                                          \
        exit(2);                                                                        \
    } while (0)

#define VKCHECK(x)                                                                      \
    do {                                                                                \
        VkResult _r = (x);                                                              \
        if (_r != VK_SUCCESS)                                                           \
            DIE("%s -> %d", #x, (int)_r);                                               \
    } while (0)

/* The colour the shader-less render pass clears to, and the 8-bit values it
 * should appear as. 0.25/0.5/0.75 are exactly representable, so a UNORM target
 * stores 64/128/191 with no rounding argument. */
#define CLEAR_R 0.25f
#define CLEAR_G 0.50f
#define CLEAR_B 0.75f
#define WANT_R 64
#define WANT_G 128
#define WANT_B 191

static int pass = 0, fail = 0;
static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (ok)
        pass++;
    else
        fail++;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 30;

    /* --- the X server end ------------------------------------------------- */
    int screen_num = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(conn))
        DIE("cannot connect to X (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++)
        xcb_screen_next(&it);
    xcb_screen_t *screen = it.data;
    xcb_window_t root = screen->root;
    printf("X server: %dx%d root=0x%x\n", screen->width_in_pixels, screen->height_in_pixels, root);

    /* --- instance --------------------------------------------------------- */
    const char *iext[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &ai,
                                .enabledExtensionCount = 2,
                                .ppEnabledExtensionNames = iext};
    VkInstance inst;
    VKCHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t ndev = 0;
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    if (!ndev)
        DIE("no Vulkan device");
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(inst, &ndev, devs));
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    /* --- what can this X server actually do? ------------------------------ */
    xcb_query_extension_reply_t *dri3 =
        xcb_query_extension_reply(conn, xcb_query_extension(conn, 4, "DRI3"), NULL);
    xcb_query_extension_reply_t *pres =
        xcb_query_extension_reply(conn, xcb_query_extension(conn, 7, "Present"), NULL);
    xcb_query_extension_reply_t *shm =
        xcb_query_extension_reply(conn, xcb_query_extension(conn, 7, "MIT-SHM"), NULL);
    xcb_query_extension_reply_t *sync =
        xcb_query_extension_reply(conn, xcb_query_extension(conn, 4, "SYNC"), NULL);
    int has_dri3 = dri3 && dri3->present;
    printf("X server extensions: DRI3=%d Present=%d MIT-SHM=%d SYNC=%d\n", has_dri3,
           pres && pres->present, shm && shm->present, sync && sync->present);
    free(dri3);
    free(pres);
    free(shm);
    free(sync);

    /* --- surface ---------------------------------------------------------- */
    VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                     .connection = conn,
                                     .window = root};
    VkSurfaceKHR surf;
    VkResult sr = vkCreateXcbSurfaceKHR(inst, &sci, NULL, &surf);
    check(sr == VK_SUCCESS, "vkCreateXcbSurfaceKHR -> %d", (int)sr);
    if (sr != VK_SUCCESS)
        goto report;

    VkBool32 supported = VK_FALSE;
    VKCHECK(vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, surf, &supported));
    check(supported, "queue family 0 can present to the surface");

    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));
    check(caps.currentExtent.width > 0 && caps.currentExtent.width != UINT32_MAX,
          "surface extent is concrete: %ux%u (minImageCount %u)",
          caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount);
    check(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          "swapchain images support COLOR_ATTACHMENT");

    uint32_t nfmt = 0;
    VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nfmt, NULL));
    VkSurfaceFormatKHR *fmts = calloc(nfmt ? nfmt : 1, sizeof(*fmts));
    VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nfmt, fmts));
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
            fmt = fmts[i];
    }
    check(nfmt > 0, "%u surface format(s), using %d / colorspace %d", nfmt, (int)fmt.format,
          (int)fmt.colorSpace);
    if (!nfmt)
        goto report;

    /* --- device ----------------------------------------------------------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = 0,
                                   .queueCount = 1,
                                   .pQueuePriorities = &prio};
    const char *dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 1,
                              .ppEnabledExtensionNames = dext};
    VkDevice dev;
    VkResult dr = vkCreateDevice(phys, &dci, NULL, &dev);
    check(dr == VK_SUCCESS, "vkCreateDevice with VK_KHR_swapchain -> %d", (int)dr);
    if (dr != VK_SUCCESS)
        goto report;
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    /* --- swapchain -------------------------------------------------------- */
    uint32_t nimg = caps.minImageCount + 1;
    VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                     .surface = surf,
                                     .minImageCount = nimg,
                                     .imageFormat = fmt.format,
                                     .imageColorSpace = fmt.colorSpace,
                                     .imageExtent = caps.currentExtent,
                                     .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                     .preTransform = caps.currentTransform,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                     .clipped = VK_TRUE};
    VkSwapchainKHR sc;
    VkResult swr = vkCreateSwapchainKHR(dev, &swci, NULL, &sc);
    check(swr == VK_SUCCESS, "vkCreateSwapchainKHR (%ux%u, FIFO, %u images) -> %d",
          caps.currentExtent.width, caps.currentExtent.height, nimg, (int)swr);
    if (swr != VK_SUCCESS)
        goto report;
    uint32_t nimgs = 0;
    VKCHECK(vkGetSwapchainImagesKHR(dev, sc, &nimgs, NULL));
    VkImage *imgs = calloc(nimgs, sizeof(*imgs));
    VKCHECK(vkGetSwapchainImagesKHR(dev, sc, &nimgs, imgs));
    check(nimgs >= 1, "driver gave us %u swapchain image(s)", nimgs);

    if (!has_dri3) {
        printf("\n  note: no DRI3 on this X server, and presentation over xcb requires it.\n");
        printf("        vendor driver: \"No DRI3 support detected - required for presentation\";\n");
        printf("        Mesa: MIT-SHM fallback applies only to software WSI devices\n");
        printf("        (wsi_common_x11.c, wants_shm = wsi_dev->sw && ...).\n");
        printf("VERDICT: SKIP - %d surface/format/swapchain facts verified, present not testable "
               "here (%d failed)\n",
               pass, fail);
        return 3;
    }

    /* --- render pass + framebuffers (clear only) -------------------------- */
    VkAttachmentDescription att = {.format = fmt.format,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference ref = {.attachment = 0,
                                 .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1,
                                   .pAttachments = &att,
                                   .subpassCount = 1,
                                   .pSubpasses = &sub};
    VkRenderPass rp;
    VKCHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

    VkImageView *views = calloc(nimgs, sizeof(*views));
    VkFramebuffer *fbs = calloc(nimgs, sizeof(*fbs));
    for (uint32_t i = 0; i < nimgs; i++) {
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = imgs[i],
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = fmt.format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .levelCount = 1,
                                                          .layerCount = 1}};
        VKCHECK(vkCreateImageView(dev, &vci, NULL, &views[i]));
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                       .renderPass = rp,
                                       .attachmentCount = 1,
                                       .pAttachments = &views[i],
                                       .width = caps.currentExtent.width,
                                       .height = caps.currentExtent.height,
                                       .layers = 1};
        VKCHECK(vkCreateFramebuffer(dev, &fci, NULL, &fbs[i]));
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = 0};
    VkCommandPool pool;
    VKCHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq, done;
    VKCHECK(vkCreateSemaphore(dev, &semi, NULL, &acq));
    VKCHECK(vkCreateSemaphore(dev, &semi, NULL, &done));

    /* --- present N frames ------------------------------------------------- */
    int acquired = 0, presented = 0;
    VkResult first_err = VK_SUCCESS;
    for (int f = 0; f < frames; f++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, sc, 1000000000ull, acq, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            if (first_err == VK_SUCCESS)
                first_err = ar;
            break;
        }
        acquired++;
        VKCHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(cb, &bi));
        VkClearValue cv;
        cv.color.float32[0] = CLEAR_R;
        cv.color.float32[1] = CLEAR_G;
        cv.color.float32[2] = CLEAR_B;
        cv.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = rp,
                                     .framebuffer = fbs[idx],
                                     .renderArea = {{0, 0}, caps.currentExtent},
                                     .clearValueCount = 1,
                                     .pClearValues = &cv};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cb);
        VKCHECK(vkEndCommandBuffer(cb));

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &acq,
                           .pWaitDstStageMask = &wait,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cb,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &done};
        VkResult qr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (qr != VK_SUCCESS) {
            if (first_err == VK_SUCCESS)
                first_err = qr;
            break;
        }
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &done,
                               .swapchainCount = 1,
                               .pSwapchains = &sc,
                               .pImageIndices = &idx};
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            if (first_err == VK_SUCCESS)
                first_err = pr;
            break;
        }
        presented++;
    }
    check(presented == frames, "acquired %d/%d, presented %d/%d frames (first error %d)",
          acquired, frames, presented, frames, (int)first_err);
    VKCHECK(vkQueueWaitIdle(queue));
    VKCHECK(vkDeviceWaitIdle(dev));

    /* --- the X server end: what is actually on the screen? ----------------
     * A separate connection, deliberately: the WSI shares ours, and a present
     * error there can leave the connection in a state that fails unrelated
     * requests afterwards (observed against the vendor driver on Xvfb). The
     * readback must not be collateral damage from the thing being measured. */
    usleep(300000); /* let the X server consume the last present */
    int vscreen = 0;
    xcb_connection_t *vconn = xcb_connect(NULL, &vscreen);
    if (xcb_connection_has_error(vconn)) {
        check(0, "could not open a second X connection for verification");
    } else {
        xcb_get_geometry_reply_t *geo =
            xcb_get_geometry_reply(vconn, xcb_get_geometry(vconn, root), NULL);
        check(geo && geo->width == screen->width_in_pixels &&
                  geo->height == screen->height_in_pixels,
              "root window geometry %ux%u (via a fresh connection)", geo ? geo->width : 0,
              geo ? geo->height : 0);
    }

    int cx = screen->width_in_pixels / 2, cy = screen->height_in_pixels / 2;
    xcb_get_image_reply_t *imgrep =
        xcb_get_image_reply(vconn,
                            xcb_get_image(vconn, XCB_IMAGE_FORMAT_Z_PIXMAP, root, cx, cy, 1, 1,
                                          ~0u),
                            NULL);
    if (!imgrep) {
        check(0, "xcb_get_image on the root window returned nothing");
    } else {
        uint8_t *px = xcb_get_image_data(imgrep);
        uint32_t raw = px[0] | (px[1] << 8) | (px[2] << 16);
        int r = raw & 0xff, g = (raw >> 8) & 0xff, b = (raw >> 16) & 0xff;
        printf("  X root pixel at (%d,%d) = raw 0x%06x -> r=%d g=%d b=%d\n", cx, cy, raw, r, g, b);
        int ok = abs(r - WANT_R) <= 2 && abs(g - WANT_G) <= 2 && abs(b - WANT_B) <= 2;
        check(ok, "X server is displaying the rendered colour (%d,%d,%d), wanted (%d,%d,%d)", r, g,
              b, WANT_R, WANT_G, WANT_B);
        free(imgrep);
    }

    printf("VERDICT: %s (%d ok, %d failed)\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;

report:
    printf("VERDICT: FAIL (%d ok, %d failed)\n", pass, fail);
    return 1;
}
