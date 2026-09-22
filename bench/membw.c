/* membw.c — memory-hierarchy bandwidth probe for the A733 (DSU/L3 sensitivity).
 *
 *   gcc -O3 -fopenmp -march=native membw.c -o membw -lm
 *   ./membw            # default: L3-resident + DRAM streaming, 1 and N threads
 *
 * Reports GB/s for:
 *   l3read   — repeated sum over arrays sized to sit in L3 (DSU-sensitive)
 *   l3shared — all threads sum ONE shared array (coherency/DSU traffic)
 *   dramread — streaming read of a buffer much larger than L3
 *   dramcopy — memcpy-style copy (read+write) of the large buffer
 * Smaller l3read/l3shared numbers relative to dramread point at the DSU/L3 clock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double sum_array(const double *a, size_t n) {
    /* 4 independent accumulators so the loop is bound by load bandwidth,
       not by FP-add latency (compile with -ffast-math for vectorisation). */
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i]; s1 += a[i + 1]; s2 += a[i + 2]; s3 += a[i + 3];
    }
    for (; i < n; i++) s0 += a[i];
    return s0 + s1 + s2 + s3;
}

int main(int argc, char **argv) {
    /* L3 on this SoC is a few MB; 2 MB x 4 arrays stays resident. */
    const size_t l3n = (argc > 1 ? atol(argv[1]) : 2) * 1024 * 1024 / sizeof(double);
    const size_t drn = (argc > 2 ? atol(argv[2]) : 256) * 1024 * 1024 / sizeof(double);
    int nthreads = omp_get_max_threads();

    double *a = malloc(l3n * sizeof(double));
    double *b = malloc(l3n * sizeof(double));
    if (!a || !b) { fprintf(stderr, "alloc failed\n"); return 1; }
    for (size_t i = 0; i < l3n; i++) { a[i] = i * 1.5; b[i] = i * 0.5; }

    double *big = malloc(drn * sizeof(double));
    double *dst = malloc(drn * sizeof(double));
    if (!big || !dst) { fprintf(stderr, "big alloc failed\n"); return 1; }
    memset(big, 1, drn * sizeof(double));

    volatile double sink = 0;

    /* --- L3 resident, single thread --- */
    double t0 = now();
    int reps = 40;
    for (int r = 0; r < reps; r++) sink += sum_array(a, l3n) + sum_array(b, l3n);
    double dt = now() - t0;
    printf("l3read.1t.GBps:      %.2f\n", (2.0 * reps * l3n * sizeof(double)) / dt / 1e9);

    /* --- L3 resident, all threads on the SAME array (coherency traffic) --- */
    t0 = now();
    #pragma omp parallel for reduction(+:sink)
    for (int r = 0; r < reps; r++) sink += sum_array(a, l3n);
    dt = now() - t0;
    printf("l3shared.%dt.GBps:   %.2f\n", nthreads, (reps * l3n * sizeof(double)) / dt / 1e9);

    /* --- DRAM streaming read --- */
    t0 = now();
    sink += sum_array(big, drn);
    dt = now() - t0;
    printf("dramread.1t.GBps:    %.2f\n", (drn * sizeof(double)) / dt / 1e9);

    /* --- DRAM copy (read + write) --- */
    t0 = now();
    memcpy(dst, big, drn * sizeof(double));
    dt = now() - t0;
    printf("dramcopy.1t.GBps:    %.2f\n", (2.0 * drn * sizeof(double)) / dt / 1e9);

    /* --- DRAM streaming read, all threads --- */
    double tb = 0;
    t0 = now();
    #pragma omp parallel for reduction(+:tb)
    for (size_t i = 0; i < drn; i++) tb += big[i];
    dt = now() - t0;
    printf("dramread.%dt.GBps:   %.2f\n", nthreads, (drn * sizeof(double)) / dt / 1e9);

    printf("threads: %d  l3_array_MB: %zu  dram_array_MB: %zu\n",
           nthreads, l3n * sizeof(double) >> 20, drn * sizeof(double) >> 20);
    (void)sink;
    return 0;
}
