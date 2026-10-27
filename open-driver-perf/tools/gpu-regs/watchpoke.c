/* Wait for a native TA kick to be pending (firmware mirror 0xd20=1) and then issue a direct
 * MTS_SCHEDULE request for DM2, watching whether the TA hardware responds. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define GPU_BASE 0x1800000ULL
#define MAP_SIZE 0x100000ULL
static volatile uint32_t *p;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc, char **argv) {
   uint32_t sched = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 16) : 0x102;
   uint32_t r130, rd20, r270, r2f8, r310, r318, r1a8;
   double t0;
   int fd, i, spins = 0;
   fd = open("/dev/mem", O_RDWR | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   p = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPU_BASE);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   t0 = now();
   /* first wait for a clean (0) kick-pending state, then for a fresh pending kick */
   while (now() - t0 < 5.0 && p[0xd20/4]) { }
   printf("baseline d20=%08x at %.3fs\n", p[0xd20/4], now()-t0);
   while (now() - t0 < 60.0) {
      if (p[0xd20/4]) break;
      spins++;
   }
   printf("pending=%08x after %.3fs spins=%d\n", p[0xd20/4], now()-t0, spins);
   r130 = p[0x130/4]; rd20 = p[0xd20/4];
   printf("before poke: 130=%08x d20=%08x\n", r130, rd20);
   p[0xb00/4] = sched;
   __sync_synchronize();
   printf("poked 0xb00=%08x (readback %08x) at %.3fs\n", sched, p[0xb00/4], now()-t0);
   r270 = p[0x270/4]; r2f8 = p[0x2f8/4]; r310 = p[0x310/4]; r318 = p[0x318/4]; r1a8 = p[0x1a8/4];
   for (i = 0; i < 10000; i++) {
      uint32_t a = p[0x130/4], d = p[0xd20/4];
      if (a != r130 || d != rd20) {
         printf("t=%.6f 130=%08x d20=%08x\n", now()-t0, a, d);
         r130 = a; rd20 = d;
      }
      if (i % 1000 == 0) {
         printf("sample t=%.4f 130=%08x d20=%08x 1a8=%08x 270=%08x 2f8=%08x 310=%08x 318=%08x\n",
                now()-t0, p[0x130/4], p[0xd20/4], p[0x1a8/4], p[0x270/4], p[0x2f8/4],
                p[0x310/4], p[0x318/4]);
      }
      usleep(100);
   }
   printf("final: 130=%08x d20=%08x 1a8=%08x 270=%08x 2f8=%08x 310=%08x 318=%08x\n",
          p[0x130/4], p[0xd20/4], p[0x1a8/4], p[0x270/4], p[0x2f8/4], p[0x310/4], p[0x318/4]);
   munmap((void *)p, MAP_SIZE); close(fd); return 0;
}
