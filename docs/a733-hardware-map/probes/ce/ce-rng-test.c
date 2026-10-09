/* ce-rng-test — does the newly enabled crypto engine EXECUTE?
 * The one test that separates "module loaded and /dev/ce exists" from "the silicon works". */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>

typedef struct {
	uint8_t *src_buffer; uint32_t src_length;
	uint8_t *dst_buffer; uint32_t dst_length;
	uint8_t *key_buffer; uint32_t key_length;
	uint8_t *iv_buffer;  uint32_t iv_length;
	uint32_t trng; uint32_t reload_offset;
	unsigned long src_phy, dst_phy, iv_phy, key_phy;
	int32_t channel_id;
} crypto_rng_req_ctx_t;

#define CE_IOC_MAGIC 'C'
#define CE_IOC_REQUEST    _IOR(CE_IOC_MAGIC, 0, int)
#define CE_IOC_FREE       _IOW(CE_IOC_MAGIC, 1, int)
#define CE_IOC_RNG_CRYPTO _IOW(CE_IOC_MAGIC, 5, crypto_rng_req_ctx_t)

static int entropy(uint8_t *b, int n) {
	int seen[256] = {0}, uniq = 0, zero = 0;
	for (int i = 0; i < n; i++) { if (!seen[b[i]]) { seen[b[i]] = 1; uniq++; } if (!b[i]) zero++; }
	printf("      unique=%d/%d zero=%d first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
	       uniq, n, zero, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
	return uniq;
}

int main(void) {
	int fd = open("/dev/ce", O_RDWR);
	if (fd < 0) { perror("open /dev/ce"); return 2; }
	printf("  open /dev/ce: ok\n");

	int ch = -1;
	if (ioctl(fd, CE_IOC_REQUEST, &ch) < 0) { perror("CE_IOC_REQUEST"); return 3; }
	printf("  CE_IOC_REQUEST -> channel_id=%d\n", ch);

	uint8_t buf[64];
	for (int run = 1; run <= 2; run++) {
		memset(buf, 0xAA, sizeof buf);          /* poison: any output must overwrite it */
		crypto_rng_req_ctx_t req;
		memset(&req, 0, sizeof req);
		req.dst_buffer = buf;
		req.dst_length = sizeof buf;
		req.trng = 1;                            /* ask for true random */
		req.channel_id = ch;
		errno = 0;
		int r = ioctl(fd, CE_IOC_RNG_CRYPTO, &req);
		printf("  RNG run %d: ret=%d errno=%d (%s)\n", run, r, errno, strerror(errno));
		entropy(buf, sizeof buf);
	}
	ioctl(fd, CE_IOC_FREE, &ch);
	close(fd);
	return 0;
}
