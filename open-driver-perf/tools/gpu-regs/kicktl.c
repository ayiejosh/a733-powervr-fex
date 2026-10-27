/* Capture a timeline of GPU MMIO 0x0-0x500 at high rate right after kick-pending (0xd20 0->1).
 * Output: binary header {u32 nsnap, u32 ndw, u64 ts0; then per snapshot: u64 ts_ns, u32 dw[ndw]} . */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define NDW 0x400 /* 0x1000 bytes: system window + MTS/execution-core registers */
static uint64_t ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
int main(int argc, char **argv) {
   unsigned nsnap = argc > 1 ? strtoul(argv[1], NULL, 0) : 3000;
   int fd = open("/dev/mem", O_RDONLY | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   volatile uint32_t *p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1800000UL);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   while (p[0xd20 / 4] == 0) { }
   uint64_t ts0 = ns(), *ts = malloc(nsnap * 8);
   uint32_t *snaps = malloc((size_t)nsnap * NDW * 4);
   for (unsigned i = 0; i < nsnap; i++) {
      ts[i] = ns();
      for (unsigned j = 0; j < NDW; j++) snaps[(size_t)i * NDW + j] = p[j];
   }
   fwrite(&nsnap, 4, 1, stdout); fwrite(&(uint32_t){NDW}, 4, 1, stdout); fwrite(&ts0, 8, 1, stdout);
   for (unsigned i = 0; i < nsnap; i++) { fwrite(&ts[i], 8, 1, stdout); fwrite(&snaps[(size_t)i * NDW], 4, NDW, stdout); }
   return 0;
}
