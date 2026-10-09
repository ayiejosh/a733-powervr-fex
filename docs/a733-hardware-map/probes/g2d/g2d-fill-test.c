/* g2d-fill-test — the real proof: does the g2d ENGINE execute, not just answer ioctl?
 *
 *  1. allocate a dma-buf from /dev/dma_heap/system and mmap it
 *  2. poison it on the CPU (so any change must be g2d's doing)
 *  3. ask g2d to FILLRECT_H it with a known colour via the fd path
 *  4. read back on the CPU and compare
 *
 * PASS only if the fill colour actually landed. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/sunxi-g2d.h>

/* dma-heap uapi (kept local so this builds without the kernel's dma-heap header) */
struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

#define W 256
#define H 256
#define SZ (W * H * 4)
#define FILL 0x00AB12CDu          /* ARGB8888; alpha 0 keeps it opaque-agnostic */

int main(void)
{
	/* 1. dma-buf */
	int hfd = open("/dev/dma_heap/system", O_RDONLY);
	if (hfd < 0) { perror("open /dev/dma_heap/system"); return 2; }
	struct dma_heap_allocation_data a = { .len = SZ, .fd_flags = O_RDWR | O_CLOEXEC };
	if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); return 2; }
	uint32_t *buf = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
	if (buf == MAP_FAILED) { perror("mmap dma-buf"); return 2; }
	printf("dma-buf        : %d bytes, fd=%u, mapped at %p\n", SZ, a.fd, (void *)buf);

	/* 2. poison */
	for (int i = 0; i < W * H; i++) buf[i] = 0xDEADBEEFu;
	printf("poisoned       : buf[0]=0x%08x  (any other value must be g2d)\n", buf[0]);

	/* 3. ask g2d to fill it */
	int fd = open("/dev/g2d", O_RDWR);
	if (fd < 0) { perror("open /dev/g2d"); return 2; }
	g2d_fillrect_h f; memset(&f, 0, sizeof f);
	f.dst_image_h.format        = G2D_FORMAT_ARGB8888;
	f.dst_image_h.width         = W;
	f.dst_image_h.height        = H;
	f.dst_image_h.fd            = a.fd;
	f.dst_image_h.use_phy_addr  = 0;
	f.dst_image_h.color         = FILL;
	f.dst_image_h.alpha         = 255;

	errno = 0;
	int r = ioctl(fd, G2D_CMD_FILLRECT_H, &f);
	printf("FILLRECT_H     : ret=%d errno=%d (%s)\n", r, errno, strerror(errno));

	/* 4. read back */
	int changed = 0;
	for (int i = 0; i < W * H; i++) if (buf[i] != 0xDEADBEEFu) { changed++; }
	printf("readback       : buf[0]=0x%08x  buf[last]=0x%08x  changed_px=%d/%d\n",
	       buf[0], buf[W * H - 1], changed, W * H);
	printf("irq 484 before : (see /proc/interrupts)\n");

	if (r == 0 && buf[0] == FILL && changed == W * H)
		printf("\nPASS: g2d executed the fill — every pixel is the fill colour\n");
	else if (changed > 0)
		printf("\nPARTIAL: g2d touched %d px but not as expected (got 0x%08x want 0x%08x)\n",
		       changed, buf[0], FILL);
	else
		printf("\nFAIL: g2d changed nothing (ioctl ret=%d)\n", r);

	munmap(buf, SZ); close(a.fd); close(hfd); close(fd);
	return (r == 0 && buf ? 0 : 1);
}
