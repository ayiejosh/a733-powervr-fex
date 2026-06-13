/* CPU upper-bound for the SAME per-pixel shader math as glbench.c's fragment shader.
   All cores (OpenMP), -O3 -ffast-math -> a generous best-case for software rendering
   (pure ALU, no rasterization/framebuffer overhead). Reports Mpix/s. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <omp.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
int main(int argc,char**argv){
    int W=1280,H=720, iters=argc>1?atoi(argv[1]):64, frames=argc>2?atoi(argv[2]):20;
    volatile double sink=0; double t0=now();
    for(int f=0; f<frames; f++){
        double fs=0;
        #pragma omp parallel for reduction(+:fs) schedule(static)
        for(int y=0;y<H;y++){
            for(int x=0;x<W;x++){
                float cx=x*0.001f, cy=y*0.001f, a=(float)f;
                for(int i=0;i<iters;i++){
                    a=a*1.0001f+sinf(cx+a)*cosf(cy-a)+(a*1.3f-floorf(a*1.3f));
                    cx+=a*0.0001f; cy+=-a*0.0001f;
                }
                fs+=a;
            }
        }
        sink+=fs;
    }
    double dt=now()-t0; double fps=frames/dt, mpix=fps*W*H/1e6;
    printf("CPU(%d threads) %dx%d loop=%d: %d frames %.3fs -> %.1f fps, %.0f Mpix/s (sink=%g)\n",
           omp_get_max_threads(),W,H,iters,frames,dt,fps,mpix,sink);
    return 0;
}
