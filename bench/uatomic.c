#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char**argv){
  long N   = argc>1?atol(argv[1]):3000000;
  int  off = argc>2?atoi(argv[2]):0;     // 0=aligned, 2=unaligned<16B, 14=crosses 16B (split-lock)
  static _Alignas(64) unsigned char buf[256];
  volatile int* p = (volatile int*)(buf+off);
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(long i=0;i<N;i++) __sync_fetch_and_add((int*)p,1);   // emits LOCK xadd
  clock_gettime(CLOCK_MONOTONIC,&b);
  double s=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
  printf("off=%-2d N=%ld  %.3fs  %.2f Mops/s\n",off,N,s,N/s/1e6);
  return 0;
}
