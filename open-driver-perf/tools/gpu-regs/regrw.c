/* Read/write GPU registers through /dev/mem for controlled MTS experiments. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define GPU_BASE 0x1800000ULL
#define MAP_SIZE 0x100000ULL
int main(int argc, char **argv) {
   volatile uint32_t *p;
   int fd, wr = 0;
   uint32_t off, val = 0;
   if (argc < 2) { printf("usage: regrw <hexoff> [value]\n"); return 2; }
   off = (uint32_t)strtoul(argv[1], NULL, 16);
   if (argc > 2) { wr = 1; val = (uint32_t)strtoul(argv[2], NULL, 16); }
   fd = open("/dev/mem", wr ? (O_RDWR | O_SYNC) : (O_RDONLY | O_SYNC));
   if (fd < 0) { perror("open"); return 1; }
   p = mmap(NULL, MAP_SIZE, wr ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, GPU_BASE);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   if (wr) {
      p[off / 4] = val;
      __sync_synchronize();
      printf("write [%04x]=%08x readback=%08x\n", off, val, p[off / 4]);
   } else {
      printf("[%04x]=%08x\n", off, p[off / 4]);
   }
   munmap((void *)p, MAP_SIZE); close(fd); return 0;
}
