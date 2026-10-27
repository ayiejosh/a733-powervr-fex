/* Try to make MTS start DM2 directly by writing documented MTS_SCHEDULE values during the native pending
 * window (bypassing the firmware-private 0xCE0 request path). */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char**argv){
   const char *probe=argv[1];
   int fd=open("/dev/mem",O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
   volatile uint32_t *p=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0x1800000UL);
   if(p==MAP_FAILED){perror("mmap");return 1;}
   pid_t c=fork(); if(c==0){freopen("/dev/null","w",stdout); setenv("PVR_PROBE_GEOM_ONLY","1",1); execl(probe,probe,NULL); _exit(127);}
   uint64_t t0=0; {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);t0=(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
   while(1){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);uint64_t n=(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; if(n-t0>3000000000ull)break; if(p[0xd20/4])break;}
   printf("pending d20=%08x sched_en=%02x b00=%03x\n",p[0xd20/4],p[0xbc8/4]&0xff,p[0xb00/4]&0x1ff);
   unsigned vals[]={0x2,0x12,0x22,0x32,0x42,0x52,0x102,0x112,0x4,0x14};
   for(unsigned i=0;i<sizeof(vals)/sizeof(vals[0]);i++){
      p[0xb00/4]=vals[i];
      usleep(200);
      printf("w b00=%03x -> read %03x ev=%08x ba0=%04x 1a8=%08x\n",vals[i],p[0xb00/4]&0x1ff,p[0x130/4],p[0xba0/4]&0xffff,p[0x1a8/4]);
   }
   waitpid(c,NULL,0); return 0;
}
