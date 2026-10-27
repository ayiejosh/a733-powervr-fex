#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv) {
   volatile uint32_t *p;
   uint64_t base = 0x1800000ULL;
   int fd = open("/dev/mem", O_RDONLY | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, base);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   printf("0x130=%08x 0xb00=%08x 0xb30=%08x 0xb38=%08x 0xbc8=%08x 0xba0=%08x 0xbd8=%08x\n",
          p[0x130/4], p[0xb00/4], p[0xb30/4], p[0xb38/4], p[0xbc8/4], p[0xba0/4], p[0xbd8/4]);
   munmap((void *)p, 0x1000); close(fd); return 0;
}
