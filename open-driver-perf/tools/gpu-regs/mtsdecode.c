/* Decode MTS registers (vendor DDK rgx_cr_defs_km.h semantics) during a kick.
 * usage: mtsdecode snap | mtsdecode watch <sec> | mtsdecode kick <probe> */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile uint32_t *p;
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static uint32_t rd(unsigned o){return p[o/4];}
static void decode(const char*tag,uint64_t t0){
   uint32_t s0=rd(0xb00), s1=rd(0x10b00), a0=rd(0xb30), a1=rd(0xb38), b0=rd(0xba0), c0=rd(0xbc8), gs=rd(0xbb0), cs=rd(0xba8), ic=rd(0xb98), en2=rd(0xb68);
   printf("%s @%.1fus s0=%03x[DM%d c%d ctx%d prio%d host%d] s1=%03x assocT0=%04x assocT1=%04x "
          "BGCTX=%04x[noncnt=%02x thract=%d dmptr=%02x] INTCTX=%04x cntSched=%02x GpuIntSt=%02x SCHED_EN=%02x DM2IntEn=%02x\n",
      tag,(ns()-t0)/1000.0, s0&0x1ff, s0&0xf, (s0>>4)&1,(s0>>5)&1,(s0>>6)&3,(s0>>8)&1,
      s1&0x1ff, a0, a1, b0&0xffff, b0&0xff,(b0>>8)&3,(b0>>10)&0x3f, ic&0xffff, cs&0xff, gs&0xff, c0&0xff, en2&0xff);
}
static unsigned regs[]={0xb00,0x10b00,0xb30,0xb38,0xba0,0xbc8,0xbb0,0xba8,0xb98,0xb68};
int main(int argc,char**argv){
   int fd=open("/dev/mem",O_RDONLY|O_SYNC); if(fd<0){perror("open");return 1;}
   p=mmap(NULL,0x20000,PROT_READ,MAP_SHARED,fd,0x1800000UL);
   if(p==MAP_FAILED){perror("mmap");return 1;}
   if(argc>1&&!strcmp(argv[1],"snap")){ decode("snap",ns()); return 0; }
   if(argc>2&&!strcmp(argv[1],"watch")){
      double secs=atof(argv[2]); uint64_t t0=ns(); uint32_t prev[16]; for(unsigned i=0;i<10;i++) prev[i]=rd(regs[i]);
      while(ns()-t0<(uint64_t)(secs*1e9)){ unsigned ch=0; for(unsigned i=0;i<10;i++){ if(rd(regs[i])!=prev[i]) ch=1; } if(ch){ decode("chg",t0); for(unsigned i=0;i<10;i++) prev[i]=rd(regs[i]); } }
      decode("end",t0); return 0; }
   if(argc>2&&!strcmp(argv[1],"kick")){
      pid_t c=fork(); if(c==0){ setenv("PVR_PROBE_GEOM_ONLY","1",1); execl(argv[2],argv[2],NULL); _exit(127); }
      uint64_t t0=ns(); while(ns()-t0<3000000000ull && !rd(0xd20)){} decode("kickpend",t0);
      uint64_t t1=ns(); uint32_t prev[16]; for(unsigned i=0;i<10;i++) prev[i]=rd(regs[i]);
      while(ns()-t1<300000000ull){ unsigned ch=0; for(unsigned i=0;i<10;i++) if(rd(regs[i])!=prev[i]) ch=1; if(ch){ decode("chg",t0); for(unsigned i=0;i<10;i++) prev[i]=rd(regs[i]); } }
      waitpid(c,NULL,0); return 0; }
   fprintf(stderr,"usage: mtsdecode snap|watch <sec>|kick <probe>\n"); return 2;
}
