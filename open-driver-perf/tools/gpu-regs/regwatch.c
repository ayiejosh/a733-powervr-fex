/* Log MMIO register changes in a window at high rate. Args: out_file seconds base len_dwords */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char**argv){
   const char *out=argv[1]; double secs=atof(argv[2]);
   unsigned base=argc>3?strtoul(argv[3],0,0):0, ndw=argc>4?strtoul(argv[4],0,0):0x1400;
   int fd=open("/dev/mem",O_RDONLY|O_SYNC); if(fd<0){perror("open");return 1;}
   volatile uint32_t *p=mmap(NULL,ndw*4,PROT_READ,MAP_SHARED,fd,0x1800000UL+base);
   if(p==MAP_FAILED){perror("mmap");return 1;}
   uint32_t *prev=malloc(ndw*4), *cur=malloc(ndw*4);
   for(unsigned j=0;j<ndw;j++) prev[j]=p[j];
   FILE *f=fopen(out,"w"); uint64_t t0=ns();
   while((ns()-t0) < (uint64_t)(secs*1e9)) {
      for(unsigned j=0;j<ndw;j++) cur[j]=p[j];
      uint64_t t=ns()-t0;
      for(unsigned j=0;j<ndw;j++) if(cur[j]!=prev[j]) fprintf(f,"%9.1fus %04x %08x -> %08x\n", t/1000.0, base+j*4, prev[j], cur[j]);
      uint32_t *tmp=prev; prev=cur; cur=tmp;
   }
   fclose(f); return 0;
}
