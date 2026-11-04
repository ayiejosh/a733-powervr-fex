/* High-rate sampler of MTS_BGCTX (0xBA0), EVENT_STATUS (0x130), context reg 0x1A8. Args: out seconds */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char**argv){
   const char*out=argv[1]; double secs=atof(argv[2]);
   int fd=open("/dev/mem",O_RDONLY|O_SYNC); if(fd<0){perror("open");return 1;}
   volatile uint32_t*p=mmap(NULL,0x10000,PROT_READ,MAP_SHARED,fd,0x1800000UL);
   if(p==MAP_FAILED){perror("mmap");return 1;}
   FILE*f=fopen(out,"w"); uint64_t t0=ns(); 
   uint32_t pb=p[0xba0/4], pe=p[0x130/4], pc=p[0x1a8/4], pd=p[0xd20/4];
   while(ns()-t0<(uint64_t)(secs*1e9)){
      uint32_t b=p[0xba0/4], e=p[0x130/4], c=p[0x1a8/4], d=p[0xd20/4];
      if(b!=pb||e!=pe||c!=pc||d!=pd){double t=(ns()-t0)/1000.0; fprintf(f,"%8.1fus ba0=%04x(ev=%08x ctx=%08x d20=%x)\n",t,b,e,c,d); pb=b;pe=e;pc=c;pd=d;}
   }
   fclose(f); return 0;
}
