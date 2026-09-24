/*
 * pvrscanout.c - render on the open PowerVR driver and put it on the display.
 *
 * Everything else in this directory proves the GPU computes and renders. This one
 * proves the result can reach a screen, which is the capability that makes a GPU
 * usable, and it does it across two DRM devices:
 *
 *   /dev/dri/card1 (powervr)     Vulkan renders into an image and hands out the
 *                                dma-buf backing an exportable one
 *   /dev/dri/card0 (sunxi-drm)   imports that dma-buf, makes a framebuffer from
 *                                it, and scans it out on the connected connector
 *
 * The buffer the display scans is not necessarily the buffer the GPU renders
 * into: a driver may refuse to make a *linear* image a colour attachment (the
 * scanout-friendly layout) while allowing it as a copy destination. So the first
 * thing this does is ask the driver which combinations it supports, print that
 * matrix, and pick one:
 *
 *   1. LINEAR, renderable, exportable          -> render straight into it
 *   2. LINEAR, copy destination, exportable    -> render offscreen, copy into it
 *   3. OPTIMAL, copy destination, exportable   -> same, with a format modifier
 *
 * Three things are then verified, in order of how easy they are to fake:
 *   1. the pixels in the *scanned* image are correct - it is copied to a host
 *      buffer and compared against the pattern the shader computes;
 *   2. the display device accepted the buffer - drmPrimeFDToHandle and
 *      drmModeAddFB2 succeeded and the CRTC reports our framebuffer id;
 *   3. the panel shows it (the hold time exists so a human can look).
 *
 * Usage: sudo ./pvrscanout [width] [height] [hold_seconds] [display_card]
 *        defaults: 1280 720 6 /dev/dri/card0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "render_frag_spv.h"
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

static const char *vkres(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "ok";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format-not-supported";
    case VK_ERROR_INITIALIZATION_FAILED: return "init-failed";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "oom";
    default: return "error";
    }
}

static int expect_r(int v)
{
    double f = (v + 0.5) / 64.0;
    f -= floor(f);
    return (int)lround(fmin(fmax(f, 0.0), 1.0) * 255.0);
}

struct choice {
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    uint32_t drm_format;
    int renderable;   /* can we render straight into it? */
    int supported;
};

/* Ask the driver, per combination, whether an exportable image of this shape
 * exists, and how exportable it is. The RENDER flag is included in the usage so
 * VK_ERROR_FORMAT_NOT_SUPPORTED here means exactly "not as a render target". */
static int probe_plain(VkPhysicalDevice phys, VkFormat fmt, VkImageTiling tiling,
                       VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = fmt,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    return vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props) == VK_SUCCESS;
}

static int probe(VkPhysicalDevice phys, VkFormat fmt, VkImageTiling tiling,
                 VkImageUsageFlags usage, VkExternalMemoryFeatureFlags *feats_out)
{
    VkPhysicalDeviceExternalImageFormatInfo ext_fmt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext_fmt,
        .format = fmt,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage = usage,
    };
    VkExternalImageFormatProperties ext_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &ext_props,
    };
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props);
    if (feats_out)
        *feats_out = (r == VK_SUCCESS) ? ext_props.externalMemoryProperties.externalMemoryFeatures
                                       : 0;
    return r == VK_SUCCESS;
}

/* The display engine keeps scanning whatever framebuffer the CRTC was last given.
 * If this process exits while its own buffer is still being scanned, the buffer is
 * freed underneath the display engine, which then faults on every scanout:

 *   iommu_master de0_iommu: ... 0x00000000fc000000 is not mapped!
 *   Bug is in DE0 module, invalid address: ...
 *
 * That floods the kernel log (16k messages seen), the desktop cannot come back, and
 * it took a forced reboot to clear. So hand the CRTC back before exiting: disable it
 * and let X / the next user set their own mode. Both the normal exit and a signal
 * (timeout, Ctrl-C) go through here.
 */
static int g_restore_fd = -1;
static uint32_t g_restore_crtc = 0;
static uint32_t g_restore_crtc_confirm = 0;

/* Hand the display back safely. Idempotent, and safe to call from a signal handler,
 * from atexit, or before an exec that will close every fd.
 *
 * The bug this replaces: both tools used to finish with
 *     if (old && old->buffer_id) drmModeSetCrtc(fd, crtc, old->buffer_id, ...);
 * The desktop is stopped before these tools run, so old->buffer_id is X's
 * framebuffer, whose memory was freed when X exited. Pointing the CRTC at it makes
 * the display engine scan unmapped memory, which faults on every scan:
 *     iommu_master de0_iommu ... 0x0x00000000fc000000 is not mapped!
 *     Bug is in DE0 module, invalid address: 0xfc000000
 * That is a storm (26k+ messages seen), it saturates CPU 0 in the IRQ handler and
 * wedges the box until the hardware watchdog resets it. So: never point the CRTC at
 * a buffer we do not own - switch the CRTC off, *confirm* it is off, give the
 * display engine a few vblanks, and only then let the fds close.
 */
static void release_display(void)
{
    if (g_restore_fd < 0 || g_restore_crtc == 0)
        return;

    if (drmModeSetCrtc(g_restore_fd, g_restore_crtc, 0, 0, 0, NULL, 0, NULL) == 0) {
        fprintf(stderr, "display released: CRTC %u off (nothing of ours left being scanned)\n",
                g_restore_crtc);
    } else {
        fprintf(stderr, "warning: could not switch CRTC %u off: %s\n", g_restore_crtc,
                strerror(errno));
    }
    g_restore_crtc = 0;

    /* Confirm it really is off before the process can free anything. */
    for (int i = 0; i < 50; i++) {
        drmModeCrtc *c = drmModeGetCrtc(g_restore_fd, g_restore_crtc_confirm);
        if (!c)
            break;
        int off = (c->buffer_id == 0);
        drmModeFreeCrtc(c);
        if (off)
            break;
        usleep(20000);
    }
    usleep(50000);
}


static void restore_crtc(void)
{
    release_display();
}

static void restore_crtc_on_signal(int sig)
{
    release_display();
    _exit(128 + sig);
}

static void arm_crtc_restore(int fd, uint32_t crtc)
{
    g_restore_fd = fd;
    g_restore_crtc = crtc;
    g_restore_crtc_confirm = crtc;
    atexit(restore_crtc);
    signal(SIGINT, restore_crtc_on_signal);
    signal(SIGTERM, restore_crtc_on_signal);
}

int main(int argc, char **argv)
{
    uint32_t width = 1280, height = 720;
    int hold = 6;
    const char *display_card = "/dev/dri/card0";

    if (argc > 1)
        width = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        height = (uint32_t)strtoul(argv[2], NULL, 0);
    if (argc > 3)
        hold = atoi(argv[3]);
    if (argc > 4)
        display_card = argv[4];

    /* ---- Vulkan: instance + device with the external-memory extensions ---- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pvr-scanout-test",
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
        DIE("no Vulkan device (is VK_ICD_FILENAMES set to the pvr ICD?)");
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    VKCHECK(vkEnumeratePhysicalDevices(instance, &ndev, devs));
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s (api %u.%u.%u), scanout buffer %ux%u\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), width, height);

    /* ---- which exportable image can we make? ---------------------------- */
    /* Ask for the whole grid rather than guessing: base support first (no
     * external struct), then the same shape as an exportable dma-buf image.
     * The two can differ, and only the second one can reach the display. */
    const VkFormat fmts[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    const char *fmt_names[] = { "RGBA8", "BGRA8" };
    const uint32_t drm_fourccs[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888 };
    const VkImageTiling tilings[] = { VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL };
    const char *tiling_names[] = { "LINEAR", "OPTIMAL" };
    struct {
        VkImageUsageFlags usage;
        const char *name;
        int renderable;
    } usages[] = {
        { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "CA+TS", 1 },
        { VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "TD+TS", 0 },
        { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "CA", 1 },
        { VK_IMAGE_USAGE_TRANSFER_DST_BIT, "TD", 0 },
    };

    printf("image support grid (base -> exportable dma-buf):\n");
    int pick = -1;
    VkFormat pick_fmt = VK_FORMAT_UNDEFINED;
    VkImageTiling pick_tiling = VK_IMAGE_TILING_LINEAR;
    VkImageUsageFlags pick_usage = 0;
    uint32_t pick_drm = 0;
    int pick_renderable = 0;
    /* Preference: linear (scanout-friendly) before tiled, renderable before not. */
    for (int want_linear = 1; want_linear >= 0; want_linear--) {
        for (int want_render = 1; want_render >= 0; want_render--) {
            for (unsigned f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
                for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); u++) {
                    VkImageTiling t = want_linear ? VK_IMAGE_TILING_LINEAR
                                                  : VK_IMAGE_TILING_OPTIMAL;
                    if (usages[u].renderable != want_render)
                        continue;
                    int base = probe_plain(phys, fmts[f], t, usages[u].usage);
                    VkExternalMemoryFeatureFlags feats = 0;
                    int ext = probe(phys, fmts[f], t, usages[u].usage, &feats);
                    int exportable = ext && (feats & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
                    if (pick < 0 || f == 0) {
                        /* print the grid once, in a stable order */
                    }
                    if (pick < 0 && exportable) {
                        pick_fmt = fmts[f];
                        pick_tiling = t;
                        pick_usage = usages[u].usage;
                        pick_drm = drm_fourccs[f];
                        pick_renderable = usages[u].renderable;
                        pick = 1;
                    }
                }
            }
        }
    }
    for (unsigned f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
        for (unsigned t = 0; t < sizeof(tilings) / sizeof(tilings[0]); t++) {
            for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); u++) {
                int base = probe_plain(phys, fmts[f], tilings[t], usages[u].usage);
                VkExternalMemoryFeatureFlags feats = 0;
                int ext = probe(phys, fmts[f], tilings[t], usages[u].usage, &feats);
                printf("  %-6s %-8s %-6s base=%-3s exportable=%-3s%s\n", fmt_names[f],
                       tiling_names[t], usages[u].name, base ? "yes" : "no",
                       ext ? ((feats & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "yes" : "no")
                           : "no",
                       (pick == 1 && fmts[f] == pick_fmt && tilings[t] == pick_tiling &&
                        usages[u].usage == pick_usage)
                           ? "   <- chosen"
                           : "");
            }
        }
    }
    /* The query above is only advisory - see the attempt loop below, which does
     * the real thing and is what decides. */
    (void)pick;
    (void)pick_fmt;
    (void)pick_tiling;
    (void)pick_usage;
    (void)pick_drm;
    (void)pick_renderable;

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfi == UINT32_MAX)
            qfi = i;
    if (qfi == UINT32_MAX)
        DIE("no graphics queue family");

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
    PFN_vkGetImageDrmFormatModifierPropertiesEXT pGetMod =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            dev, "vkGetImageDrmFormatModifierPropertiesEXT");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    /* ---- make the image the display will scan ---------------------------
     * The capability query above is advisory: pvr implements import/export for
     * dma-buf (pvr_AllocateMemory / pvr_GetMemoryFdKHR), so the honest test is to
     * create the image, allocate exportable memory for it and ask for the fd.
     * First shape that survives all three wins. */
    const VkImageUsageFlags CA_TS = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags TD_TS = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    struct attempt {
        VkFormat fmt;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        uint32_t drm_format;
        int renderable;
        const char *name;
    } tries[] = {
        { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_LINEAR, CA_TS, DRM_FORMAT_ABGR8888, 1,
          "RGBA8 LINEAR renderable" },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_LINEAR, TD_TS, DRM_FORMAT_ABGR8888, 0,
          "RGBA8 LINEAR copy-dst" },
        { VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_LINEAR, CA_TS, DRM_FORMAT_ARGB8888, 1,
          "BGRA8 LINEAR renderable" },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, CA_TS, DRM_FORMAT_ABGR8888, 1,
          "RGBA8 OPTIMAL renderable" },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, TD_TS, DRM_FORMAT_ABGR8888, 0,
          "RGBA8 OPTIMAL copy-dst" },
    };

    VkImage scan_img = VK_NULL_HANDLE;
    VkDeviceMemory scan_mem = VK_NULL_HANDLE;
    VkMemoryRequirements scan_mr = { 0 };
    VkFormat scan_fmt = VK_FORMAT_UNDEFINED;
    VkImageTiling scan_tiling = VK_IMAGE_TILING_LINEAR;
    uint32_t drm_format = 0;
    int render_direct = 0;
    int exported_fd = -1;

    printf("trying to create an exportable image:\n");
    for (unsigned i = 0; i < sizeof(tries) / sizeof(tries[0]) && exported_fd < 0; i++) {
        VkExternalMemoryImageCreateInfo ext_img = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_img,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = tries[i].fmt,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = tries[i].tiling,
            .usage = tries[i].usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage img;
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        if (r != VK_SUCCESS) {
            printf("  %-28s vkCreateImage -> %s\n", tries[i].name, vkres(r));
            continue;
        }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, img, &mr);
        uint32_t mti = UINT32_MAX;
        for (uint32_t m = 0; m < mp.memoryTypeCount; m++)
            if (mr.memoryTypeBits & (1u << m)) {
                mti = m;
                break;
            }
        VkMemoryDedicatedAllocateInfo ded = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = img,
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
        VkDeviceMemory mem;
        r = vkAllocateMemory(dev, &mai, NULL, &mem);
        if (r != VK_SUCCESS) {
            printf("  %-28s vkAllocateMemory -> %s\n", tries[i].name, vkres(r));
            vkDestroyImage(dev, img, NULL);
            continue;
        }
        r = vkBindImageMemory(dev, img, mem, 0);
        if (r != VK_SUCCESS) {
            printf("  %-28s vkBindImageMemory -> %s\n", tries[i].name, vkres(r));
            vkDestroyImage(dev, img, NULL);
            continue;
        }
        VkMemoryGetFdInfoKHR gfi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = mem,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        int fd = -1;
        r = pGetMemoryFdKHR(dev, &gfi, &fd);
        if (r != VK_SUCCESS || fd < 0) {
            printf("  %-28s vkGetMemoryFdKHR -> %s\n", tries[i].name, vkres(r));
            vkDestroyImage(dev, img, NULL);
            continue;
        }
        printf("  %-28s WORKS (dma-buf fd=%d)%s\n", tries[i].name, fd,
               tries[i].renderable ? "" : " - needs a copy from a render image");
        scan_img = img;
        scan_mem = mem;
        scan_mr = mr;
        scan_fmt = tries[i].fmt;
        scan_tiling = tries[i].tiling;
        drm_format = tries[i].drm_format;
        render_direct = tries[i].renderable;
        exported_fd = fd;
    }
    if (exported_fd < 0)
        DIE("no exportable image shape worked - dma-buf export is not functional here");

    uint64_t modifier = 0;
    if (scan_tiling == VK_IMAGE_TILING_OPTIMAL && pGetMod) {
        VkImageDrmFormatModifierPropertiesEXT mp_ = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        };
        if (pGetMod(dev, scan_img, &mp_) == VK_SUCCESS)
            modifier = mp_.drmFormatModifier;
    }

    uint64_t row_pitch = (uint64_t)width * 4;
    uint64_t offset = 0;
    if (scan_tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(dev, scan_img, &subres, &layout);
        row_pitch = layout.rowPitch;
        offset = layout.offset;
    }
    printf("scanout image: %s %s, rowPitch=%llu offset=%llu size=%llu modifier=0x%llx\n",
           scan_fmt == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "BGRA8",
           scan_tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL",
           (unsigned long long)row_pitch, (unsigned long long)offset,
           (unsigned long long)scan_mr.size, (unsigned long long)modifier);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = scan_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = scan_fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView view;
    VKCHECK(vkCreateImageView(dev, &ivci, NULL, &view));

    VkImage render_img = scan_img;
    if (!render_direct) {
        VkImageCreateInfo r_ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = scan_fmt,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VKCHECK(vkCreateImage(dev, &r_ci, NULL, &render_img));
        VkMemoryRequirements r_mr;
        vkGetImageMemoryRequirements(dev, render_img, &r_mr);
        uint32_t r_mti = UINT32_MAX;
        for (uint32_t m = 0; m < mp.memoryTypeCount; m++)
            if (r_mr.memoryTypeBits & (1u << m)) {
                r_mti = m;
                break;
            }
        VkMemoryAllocateInfo r_mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = r_mr.size,
            .memoryTypeIndex = r_mti,
        };
        VkDeviceMemory r_mem;
        VKCHECK(vkAllocateMemory(dev, &r_mai, NULL, &r_mem));
        VKCHECK(vkBindImageMemory(dev, render_img, r_mem, 0));

        VkImageViewCreateInfo r_iv = ivci;
        r_iv.image = render_img;
        VkImageView r_view;
        VKCHECK(vkCreateImageView(dev, &r_iv, NULL, &r_view));
        view = r_view;
        printf("rendering offscreen, then copying into the scanout image\n");
    }

    /* ---- render pass + pipeline ----------------------------------------- */
    VkAttachmentDescription att = {
        .format = scan_fmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = render_direct ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
        .width = width,
        .height = height,
        .layers = 1,
    };
    VkFramebuffer fb;
    VKCHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

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

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
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
    VKCHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    /* ---- verification buffer -------------------------------------------- */
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
    if (bmti == UINT32_MAX)
        DIE("no host-visible memory for verification");
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

    /* ---- record, submit, wait ------------------------------------------- */
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
        .renderArea = { { 0, 0 }, { width, height } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (!render_direct) {
        VkImageCopy copy = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { width, height, 1 },
        };
        vkCmdCopyImage(cmd, render_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scan_img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, scan_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &host_barrier, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (w != VK_SUCCESS)
        DIE("GPU never signalled (VkResult %d)", (int)w);

    const unsigned char *px = mapped;
    uint64_t bad = 0;
    uint32_t fx = 0, fy = 0;
    int er = 0, eg = 0, gr = 0, gg = 0;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const unsigned char *p = px + ((size_t)y * width + x) * 4;
            if (p[0] != expect_r(x) || p[1] != expect_r(y) || p[2] != 64 || p[3] != 255) {
                if (bad == 0) {
                    fx = x;
                    fy = y;
                    er = expect_r(x);
                    eg = expect_r(y);
                    gr = p[0];
                    gg = p[1];
                }
                bad++;
            }
        }
    if (bad) {
        printf("RESULT: FAIL - the image to be scanned out is wrong: %llu/%u pixels (first at "
               "%u,%u want %d,%d got %d,%d)\n",
               (unsigned long long)bad, width * height, fx, fy, er, eg, gr, gg);
        return 1;
    }
    printf("1/3 render: PASS (%u pixels verified in the scanout image)\n", width * height);

    /* ---- export --------------------------------------------------------- */
    VkMemoryGetFdInfoKHR mgfi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = scan_mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int dmabuf_fd = -1;
    VkResult gfr = pGetMemoryFdKHR(dev, &mgfi, &dmabuf_fd);
    if (gfr != VK_SUCCESS)
        DIE("vkGetMemoryFdKHR -> %d", (int)gfr);
    printf("2/3 export: PASS (dma-buf fd=%d, %llu bytes)\n", dmabuf_fd,
           (unsigned long long)scan_mr.size);

    /* ---- scan out ------------------------------------------------------- */
    int dfd = open(display_card, O_RDWR | O_CLOEXEC);
    if (dfd < 0)
        DIE("cannot open display card %s: %s", display_card, strerror(errno));
    if (drmSetMaster(dfd) != 0)
        printf("note: drmSetMaster: %s (continuing)\n", strerror(errno));

    uint32_t handle = 0;
    if (drmPrimeFDToHandle(dfd, dmabuf_fd, &handle) != 0)
        DIE("drmPrimeFDToHandle on %s failed: %s", display_card, strerror(errno));
    printf("   imported into %s as GEM handle %u\n", display_card, handle);

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
        DIE("no CRTC available");
    drmModeCrtc *old = drmModeGetCrtc(dfd, crtc_id);
    arm_crtc_restore(dfd, crtc_id);

    /* Use the mode the panel is already running, so the only thing that changes
     * on screen is the content. */
    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (old && conn->modes[i].clock == old->mode.clock &&
            conn->modes[i].hdisplay == old->mode.hdisplay &&
            conn->modes[i].vdisplay == old->mode.vdisplay) {
            mode = conn->modes[i];
            break;
        }
    printf("   display: connector %u crtc %u mode %ux%u@%u (image %ux%u)\n", conn->connector_id,
           crtc_id, mode.hdisplay, mode.vdisplay, mode.vrefresh, width, height);
    if ((uint32_t)mode.hdisplay != width || (uint32_t)mode.vdisplay != height) {
        /* Nothing has been changed on the display yet, so the cheapest correct
         * answer is to redo the run at the panel's own size. */
        char wbuf[16], hbuf[16], hbuf2[16];
        snprintf(wbuf, sizeof(wbuf), "%u", mode.hdisplay);
        snprintf(hbuf, sizeof(hbuf), "%u", mode.vdisplay);
        snprintf(hbuf2, sizeof(hbuf2), "%d", hold);
        printf("   panel is %ux%u, re-running at that size\n", mode.hdisplay, mode.vdisplay);
        fflush(stdout);
        /* The exec closes every fd we hold (they are all O_CLOEXEC), which frees the
         * framebuffers - so the CRTC must not still be scanning ours when it happens,
         * or the display engine faults on the freed memory. Hand the display back
         * first; the re-executed process sets it up again from scratch. */
        release_display();
        execl("/proc/self/exe", "pvrscanout", wbuf, hbuf, hbuf2, display_card, (char *)NULL);
        DIE("execl failed: %s", strerror(errno));
    }

    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { (uint32_t)row_pitch, 0, 0, 0 };
    uint32_t offsets[4] = { (uint32_t)offset, 0, 0, 0 };
    uint32_t fb_id = 0;
    int addfb;
    if (modifier) {
        uint64_t mods[4] = { modifier, 0, 0, 0 };
        addfb = drmModeAddFB2WithModifiers(dfd, width, height, drm_format, handles, pitches,
                                           offsets, mods, &fb_id,
                                           DRM_MODE_FB_MODIFIERS);
    } else {
        addfb = drmModeAddFB2(dfd, width, height, drm_format, handles, pitches, offsets, &fb_id,
                              0);
    }
    if (addfb != 0)
        DIE("drmModeAddFB2%s failed: %s (format 0x%x pitch %u modifier 0x%llx)",
            modifier ? "WithModifiers" : "", strerror(errno), drm_format,
            (uint32_t)row_pitch, (unsigned long long)modifier);

    if (drmModeSetCrtc(dfd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, &mode) != 0)
        DIE("drmModeSetCrtc failed: %s", strerror(errno));

    drmModeCrtc *now = drmModeGetCrtc(dfd, crtc_id);
    int committed = now && now->buffer_id == fb_id;
    printf("3/3 scanout: fb %u on crtc %u - %s\n", fb_id, crtc_id,
           committed ? "committed, CRTC reports it" : "set, CRTC does not report it");

    if (hold > 0 && committed) {
        printf("   holding it on screen for %d s ...\n", hold);
        sleep(hold);
    }

    /* The old CRTC state belongs to a desktop that is no longer running, so its
     * framebuffer memory is gone: switch the CRTC off instead of pointing it at a
     * buffer that no longer exists. */
    release_display();
    if (now)
        drmModeFreeCrtc(now);
    if (old)
        drmModeFreeCrtc(old);

    printf("VERDICT: %s\n",
           committed ? "PASS - the open driver rendered a buffer the display scanned out"
                     : "PARTIAL - buffer accepted, commit not confirmed");
    return committed ? 0 : 1;
}
