/* On a firmware kick (mirror 0xd20 0->1), wait a fixed delay and snapshot the DM kick register
 * windows (0x000-0x4ff) so a working Vulkan kick and a failing native kick can be diffed at the same
 * point in the sequence. */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define GPU_BASE 0x1800000ULL
#define MAP_SIZE 0x100000ULL
static volatile uint32_t *p;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc, char **argv) {
   const unsigned delay_us = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
   const unsigned kicks_wanted = argc > 2 ? (unsigned)atoi(argv[2]) : 3;
   unsigned kicks = 0, off;
   int fd;
   fd = open("/dev/mem", O_RDONLY | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   p = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, fd, GPU_BASE);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   while (kicks < kicks_wanted) {
      double t0;
      while (p[0xd20/4] == 0) { }
      t0 = now();
      usleep(delay_us);
      printf("# kick %u t=%.6f (delay %u us) d20=%08x 130=%08x\n", kicks, now()-t0, delay_us,
             p[0xd20/4], p[0x130/4]);
      for (off = 0; off < 0x500; off += 4) {
         uint32_t v = p[off/4];
         if (v)
            printf("%03x=%08x ", off, v);
      }
      printf("\n");
      kicks++;
      while (p[0xd20/4] != 0) { }
   }
   munmap((void *)p, MAP_SIZE); close(fd); return 0;
}
