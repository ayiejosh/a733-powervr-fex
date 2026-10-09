/* ce-hash-test — is the newly enabled crypto engine CORRECT and FAST?
 * Hashes a known 1 MB buffer on the hardware and prints the digest so it can be
 * compared against sha256sum (correctness), then times N iterations (speed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

typedef struct {
	uint8_t *text_buffer; uint32_t text_length;
	uint8_t *key_buffer;  uint32_t key_length;
	uint8_t *dst_buffer;  uint32_t dst_length;
	uint8_t *iv_buffer;   uint32_t iv_length;
	uint32_t hash_mode; uint32_t ion_flag;
	unsigned long text_phy, dst_phy;
	int32_t channel_id;
} crypto_hash_req_ctx_t;

#define CE_IOC_MAGIC 'C'
#define CE_IOC_REQUEST     _IOR(CE_IOC_MAGIC, 0, int)
#define CE_IOC_FREE        _IOW(CE_IOC_MAGIC, 1, int)
#define CE_IOC_HASH_CRYPTO _IOW(CE_IOC_MAGIC, 4, crypto_hash_req_ctx_t)

#define SHA256 3
#define SZ (1024*1024)

int main(int argc, char **argv) {
	int iters = argc > 1 ? atoi(argv[1]) : 20;
	uint8_t *text = malloc(SZ), digest[32];
	for (int i = 0; i < SZ; i++) text[i] = (uint8_t)(i * 31 + 7);   /* deterministic */

	int fd = open("/dev/ce", O_RDWR);
	if (fd < 0) { perror("open /dev/ce"); return 2; }
	int ch = -1;
	if (ioctl(fd, CE_IOC_REQUEST, &ch) < 0) { perror("REQUEST"); return 3; }

	crypto_hash_req_ctx_t req;
	memset(&req, 0, sizeof req);
	req.text_buffer = text; req.text_length = SZ;
	memset(digest, 0, sizeof digest);
	req.dst_buffer = digest; req.dst_length = sizeof digest;
	req.hash_mode = SHA256; req.channel_id = ch;

	errno = 0;
	int r = ioctl(fd, CE_IOC_HASH_CRYPTO, &req);
	printf("  HASH ret=%d errno=%d (%s)\n", r, errno, strerror(errno));
	printf("  digest = ");
	for (int i = 0; i < 32; i++) printf("%02x", digest[i]);
	printf("\n");

	if (r == 0) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < iters; i++) ioctl(fd, CE_IOC_HASH_CRYPTO, &req);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("  %d iters x 1 MiB in %.3f s -> %.1f MB/s (hardware)\n",
		       iters, sec, iters / sec);
	}
	ioctl(fd, CE_IOC_FREE, &ch);
	close(fd);
	/* leave the same buffer on disk so sha256sum can verify the digest */
	FILE *f = fopen("/tmp/ce-hash-input.bin", "wb");
	if (f) { fwrite(text, 1, SZ, f); fclose(f); }
	return 0;
}
