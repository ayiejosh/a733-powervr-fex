#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char**argv){
  long N=argc>1?atol(argv[1]):20000000;
  double acc=0.0, inc=1.0000003;          /* 64-bit doubles */
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(long i=0;i<N;i++)
    __asm__ volatile("fldl %0; fldl %1; faddp %%st,%%st(1); fstpl %0":"+m"(acc):"m"(inc):"memory");
  clock_gettime(CLOCK_MONOTONIC,&b);
  double s=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
  printf("N=%ld %.3fs %.1f ns/iter acc=%.3f\n",N,s,s/N*1e9,acc);
  return 0;
}
