/* g2d-probe — prove the freshly-enabled g2d block answers ioctl.
 * The vendor UAPI is used verbatim so the ioctl numbers cannot drift;
 * -include stdbool.h is needed because the header assumes the kernel's bool. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/sunxi-g2d.h>

int main(void)
{
	int fd = open("/dev/g2d", O_RDWR);
	if (fd < 0) { perror("open /dev/g2d"); return 2; }
	printf("open /dev/g2d  : ok (fd=%d)\n", fd);

	struct g2d_hardware_version v;
	memset(&v, 0, sizeof v);
	errno = 0;
	int r = ioctl(fd, G2D_CMD_QUERY_VERSION, &v);
	printf("QUERY_VERSION  : ret=%d errno=%d (%s)\n", r, errno, strerror(errno));
	if (r == 0)
		printf("  g2d_version  = 0x%08x\n  chip_version = 0x%08x\n",
		       v.g2d_version, v.chip_version);

	close(fd);
	return r < 0 ? 1 : 0;
}
