#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdint.h>
static void *h; static int (*r_create)(int,uint32_t,uint32_t*);
static void *get(const char*n){ if(!h) h=dlopen("libdrm.so.2",RTLD_LAZY); return dlsym(h,n); }
#define MAX 512
#define DEPTH 6
static struct { unsigned long f[DEPTH]; unsigned long n; } tab[MAX]; static int nt;
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *hh) {
   if (!r_create) r_create = get("drmSyncobjCreate");
   void *b[DEPTH+2]; int d = backtrace(b, DEPTH+2);
   if (d > 2) {
      unsigned long key[DEPTH];
      for (int i = 0; i < DEPTH; i++) key[i] = i+2 < d ? (unsigned long)b[i+2] : 0;
      for (int i = 0; i < nt; i++) { int same=1; for(int j=0;j<DEPTH;j++) if(tab[i].f[j]!=key[j]) same=0; if(same){tab[i].n++;goto out;} }
      if (nt<MAX){ for(int j=0;j<DEPTH;j++) tab[nt].f[j]=key[j]; tab[nt].n=1; nt++; }
   }
out:
   return r_create(fd, flags, hh);
}
__attribute__((destructor)) static void done(void) {
   for (int i = 0; i < nt; i++) {
      fprintf(stderr,"[chain] n=%lu offs:", tab[i].n);
      for (int j = 0; j < DEPTH && tab[i].f[j]; j++) {
         Dl_info di; if (dladdr((void*)tab[i].f[j],&di) && di.dli_fbase)
            fprintf(stderr," %lx", (unsigned long)(tab[i].f[j]-(unsigned long)di.dli_fbase));
         else fprintf(stderr," ?");
      }
      fprintf(stderr,"\n");
   }
}
