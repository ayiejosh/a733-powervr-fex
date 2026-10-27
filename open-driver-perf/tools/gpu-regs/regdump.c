/* Dump GPU MMIO 0x1800000 pages (default 64 KiB) as dwords, robust to unreadable pages (SIGBUS). */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static sigjmp_buf jb;
static void onbus(int sig) { (void)sig; siglongjmp(jb, 1); }
int main(int argc, char **argv) {
   unsigned long base = 0, len = 0x10000;
   if (argc > 1) base = strtoul(argv[1], NULL, 0);
   if (argc > 2) len = strtoul(argv[2], NULL, 0);
   int fd = open("/dev/mem", O_RDONLY | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   volatile uint32_t *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0x1800000UL + base);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   struct sigaction sa = { .sa_handler = onbus };
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGSEGV, &sa, NULL);
   uint32_t *buf = malloc(0x1000);
   for (unsigned long off = 0; off < len; off += 0x1000) {
      if (sigsetjmp(jb, 1)) { printf("%08lx: <unreadable>\n", base + off); continue; }
      for (int i = 0; i < 0x1000 / 4; i++) buf[i] = p[off / 4 + i];
      for (int i = 0; i < 0x1000 / 4; i++) {
         if (i % 8 == 0) printf("%08lx:", base + off + i * 4);
         printf(" %08x", buf[i]);
         if (i % 8 == 7) printf("\n");
      }
   }
   return 0;
}
