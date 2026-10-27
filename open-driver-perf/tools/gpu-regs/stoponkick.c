/* Freeze a process the moment a firmware kick goes pending (hardware mirror 0xd20 0->1). */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv) {
   volatile uint32_t *p;
   pid_t pid = atoi(argv[1]);
   int fd = open("/dev/mem", O_RDONLY | O_SYNC);
   if (fd < 0) { perror("open"); return 1; }
   p = mmap(NULL, 0x100000, PROT_READ, MAP_SHARED, fd, 0x1800000ULL);
   if (p == MAP_FAILED) { perror("mmap"); return 1; }
   while (p[0xd20 / 4] == 0) { }
   kill(pid, SIGSTOP);
   return 0;
}
