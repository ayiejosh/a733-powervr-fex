/* Write the firmware-private client-kick register (thread<<19 + 0xCE0 = 1) during the native pending window.
 * usage: kickpoke <probe> */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(int argc,char**argv){
   const char *probe=argv[1];
   int fd=open("/dev/mem",O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
   volatile uint32_t *p=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0x1800000UL);
   if(p==MAP_FAILED){perror("mmap");return 1;}
   pid_t c=fork(); if(c==0){freopen("/dev/null","w",stdout); setenv("PVR_PROBE_GEOM_ONLY","1",1); execl(probe,probe,NULL); _exit(127);}
   uint64_t t0=ns(); while(ns()-t0<3000000000ull && !p[0xd20/4]){}
   printf("pending seen, ce0=%08x d20=%08x\n",p[0xce0/4],p[0xd20/4]);
   for(int i=0;i<100;i++){ p[0xce0/4]=1; if(i%10==0) printf("i=%d ev=%08x ba0=%08x 1a8=%08x d20=%08x\n",i,p[0x130/4],p[0xba0/4],p[0x1a8/4],p[0xd20/4]); usleep(500); }
   printf("after: ev=%08x ba0=%08x 1a8=%08x d20=%08x ce0=%08x\n",p[0x130/4],p[0xba0/4],p[0x1a8/4],p[0xd20/4],p[0xce0/4]);
   waitpid(c,NULL,0); return 0;
}
