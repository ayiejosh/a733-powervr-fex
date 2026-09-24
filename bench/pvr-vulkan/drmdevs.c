/*
 * drmdevs.c - inventory of DRM devices as Mesa sees them.
 *
 * Mesa's pvr driver decides whether it can use a GPU by matching the device tree
 * "compatible" strings of a render node and a display node against a hardcoded
 * table (src/imagination/vulkan/pvr_device.c). So the only way to know what
 * patch that table needs for a given board is to ask libdrm for exactly what
 * Mesa asks it for. This prints that, plus the drmGetVersion name/version that
 * pvr_winsys_create() switches on.
 *
 * Build: gcc -O1 -I/usr/include/libdrm -o drmdevs drmdevs.c -ldrm
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static const char *bus_name(int bustype)
{
    switch (bustype) {
    case DRM_BUS_PCI:      return "pci";
    case DRM_BUS_USB:      return "usb";
    case DRM_BUS_PLATFORM: return "platform";
    case DRM_BUS_HOST1X:   return "host1x";
    default:               return "?";
    }
}

static void print_nodes(drmDevicePtr d, const char *kind, uint32_t bit)
{
    if (!(d->available_nodes & (1u << (bit))))
        return;
    printf("  %-7s %s\n", kind, d->nodes[bit]);
}

int main(void)
{
    drmDevicePtr devs[32];
    int n = drmGetDevices2(0, devs, 32);
    if (n < 0) {
        fprintf(stderr, "drmGetDevices2 failed: %s\n", strerror(-n));
        return 1;
    }

    printf("%d DRM device(s)\n", n);
    for (int i = 0; i < n; i++) {
        drmDevicePtr d = devs[i];

        printf("device[%d] bustype=%s\n", i, bus_name(d->bustype));
        print_nodes(d, "primary", DRM_NODE_PRIMARY);
        print_nodes(d, "control", DRM_NODE_CONTROL);
        print_nodes(d, "render", DRM_NODE_RENDER);

        if (d->bustype == DRM_BUS_PLATFORM && d->deviceinfo.platform) {
            printf("  compatible:");
            for (char **c = d->deviceinfo.platform->compatible; c && *c; c++)
                printf(" \"%s\"", *c);
            printf("\n");
        }

        const char *path = NULL;
        if (d->available_nodes & (1u << DRM_NODE_RENDER))
            path = d->nodes[DRM_NODE_RENDER];
        else if (d->available_nodes & (1u << DRM_NODE_PRIMARY))
            path = d->nodes[DRM_NODE_PRIMARY];

        if (path) {
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                drmVersionPtr v = drmGetVersion(fd);
                if (v) {
                    printf("  version name=\"%s\" %d.%d.%d desc=\"%s\"\n",
                           v->name, v->version_major, v->version_minor,
                           v->version_patchlevel, v->desc ? v->desc : "");
                    drmFreeVersion(v);
                }
                close(fd);
            }
        }
        printf("\n");
    }

    drmFreeDevices(devs, n);
    return 0;
}
