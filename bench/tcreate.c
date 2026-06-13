#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
static void* worker(void* a){ return a; }
int main(int argc,char**argv){
  long N = argc>1?atol(argv[1]):10000;
  struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(long i=0;i<N;i++){ pthread_t t; pthread_create(&t,0,worker,0); pthread_join(t,0); }
  clock_gettime(CLOCK_MONOTONIC,&b);
  double s=(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9;
  printf("threads=%ld  %.3fs  %.0f ns/op\n",N,s,s/N*1e9);
  return 0;
}
