/*
 * fexbench — microbenchmarks shaped for x86-64 emulator tuning on ARM64.
 *
 * Why this exists: generic "CPU benchmarks" measure the guest libc, not the
 * translator. Each test below isolates one translation category that a FEX or
 * box64 setting actually changes:
 *
 *   alu       dependent 64-bit integer chain      -> JIT register allocation / codegen quality
 *   branch    unpredictable data-dependent branch -> JIT flag handling (SAFEFLAGS)
 *   fp_double scalar SSE2 double FMA chain        -> SSE2 translation
 *   fp_x87    80-bit x87 chain (x86 only)         -> X87ReducedPrecision
 *   memcpy    libc memcpy, 1.28 GB moved          -> MemcpySetTSOEnabled / rep movsb translation
 *   memloop   hand-written byte copy loop         -> load/store translation without libc
 *   syscall   getpid() x N                        -> syscall emulation overhead, not JIT
 *   atomics   4 threads hammering one counter     -> TSO / atomic ordering cost (checksum must be exact)
 *   threads   4 threads of pure ALU               -> big.LITTLE scheduling and per-thread JIT
 *
 * Every test ends in a checksum printed to stdout. The same checksum under
 * native ARM64 and under each x86-64 emulator proves a tuning change did not
 * buy speed with wrong results — the single most common way emulator tuning
 * silently corrupts a program.
 *
 * Build:  gcc -O2 -static -pthread -o fexbench_arm64 fexbench.c
 *         x86_64-linux-gnu-gcc -O2 -static -pthread -o fexbench_x64 fexbench.c
 * Run:    taskset -c 6 ./fexbench_arm64        (pin to one core for comparability)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __x86_64__
#define ARCH_NAME "x86_64"
#elif defined(__aarch64__)
#define ARCH_NAME "aarch64"
#else
#define ARCH_NAME "unknown"
#endif

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* Written through a volatile sink so no loop can be optimised away. */
static volatile uint64_t g_sink;
static uint64_t g_counter;

/* --- alu: one long dependency chain, no branches, no memory -------------- */
static uint64_t t_alu(void) {
    uint64_t x = 0x123456789abcdefULL;
    for (uint64_t i = 0; i < 300000000ULL; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        x ^= x >> 29;
    }
    return x;
}

/* --- branch: 50M data-dependent, unpredictable branches ------------------ */
static uint64_t t_branch(void) {
    uint64_t x = 88172645463325252ULL, c = 0;
    for (uint64_t i = 0; i < 50000000ULL; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if (x & 0x100)
            c += x;
        else
            c -= x;
    }
    return c;
}

/* --- fp_double: dependent scalar double chain (SSE2 on x86-64) ----------- */
static uint64_t t_fp_double(void) {
    double a = 1.0000001, b = 0.9999999;
    for (uint64_t i = 0; i < 100000000ULL; i++) a = a * b + 1e-9;
    g_sink = (uint64_t)a;
    return (uint64_t)(a * 1e15);
}

/* --- fp_x87: long double is x87 on x86-64, quad-float on aarch64 --------- */
static uint64_t t_fp_x87(void) {
#if defined(__x86_64__)
    long double a = 1.0000001L, b = 0.9999999L;
    for (uint64_t i = 0; i < 20000000ULL; i++) a = a * b + 1e-9L;
    g_sink = (uint64_t)a;
    return (uint64_t)(a * 1e15L);
#else
    return 0; /* not comparable: aarch64 long double is not x87 */
#endif
}

/* --- memcpy: libc bulk copy, 40 x 32 MB = 1.28 GB ------------------------ */
static uint64_t t_memcpy(void) {
    const size_t sz = 32u << 20;
    unsigned char *a = malloc(sz), *b = malloc(sz);
    if (!a || !b) return 0;
    memset(a, 0xA5, sz);
    for (int r = 0; r < 40; r++) {
        memcpy(b, a, sz);
        b[(size_t)(r * 7919) % sz]++; /* touch the destination so it cannot be elided */
    }
    uint64_t s = 0;
    for (size_t i = 0; i < sz; i += 4096) s += b[i];
    free(a);
    free(b);
    return s;
}

/* --- memloop: byte-at-a-time copy, 16 MB, no libc ------------------------ */
static uint64_t t_memloop(void) {
    const size_t sz = 16u << 20;
    unsigned char *a = malloc(sz), *b = malloc(sz);
    if (!a || !b) return 0;
    for (size_t i = 0; i < sz; i++) a[i] = (unsigned char)(i * 131 + 7);
    for (size_t i = 0; i < sz; i++) b[i] = a[i];
    uint64_t s = 0;
    for (size_t i = 0; i < sz; i += 1024) s += b[i];
    free(a);
    free(b);
    return s;
}

/* --- syscall: 200k getpid(), no JIT work involved ----------------------- */
static uint64_t t_syscall(void) {
    uint64_t s = 0;
    for (int i = 0; i < 200000; i++) s += (uint64_t)getpid();
    return s;
}

/* --- atomics: 4 threads x 5M seq_cst increments of one shared counter ---- */
#define ATOMIC_THREADS 4
#define ATOMIC_ITERS 5000000L

static void *atomic_worker(void *arg) {
    (void)arg;
    for (long i = 0; i < ATOMIC_ITERS; i++) __atomic_fetch_add(&g_counter, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static uint64_t t_atomics(void) {
    pthread_t th[ATOMIC_THREADS];
    g_counter = 0;
    for (int i = 0; i < ATOMIC_THREADS; i++) pthread_create(&th[i], NULL, atomic_worker, NULL);
    for (int i = 0; i < ATOMIC_THREADS; i++) pthread_join(th[i], NULL);
    /* must be exactly threads*iters; TSO or atomic emulation bugs break this */
    return g_counter;
}

/* --- threads: 4 threads of pure ALU, independent chains ----------------- */
#define ALU_THREADS 4
#define ALU_ITERS 100000000ULL

static void *alu_worker(void *arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg | 1;
    for (uint64_t i = 0; i < ALU_ITERS; i++) {
        x = x * 6364136223846793005ULL + 1;
        x ^= x >> 29;
    }
    __atomic_fetch_add(&g_sink, x, __ATOMIC_RELAXED);
    return NULL;
}

static uint64_t t_threads(void) {
    pthread_t th[ALU_THREADS];
    g_sink = 0;
    for (int i = 0; i < ALU_THREADS; i++)
        pthread_create(&th[i], NULL, alu_worker, (void *)(uintptr_t)(0x1000 + i * 0x100));
    for (int i = 0; i < ALU_THREADS; i++) pthread_join(th[i], NULL);
    return g_sink;
}

typedef struct {
    const char *name;
    uint64_t (*fn)(void);
    double ms;
    uint64_t sum;
} test_t;

/* Select a comma-separated subset, e.g. "alu,branch". Needed because the
 * single-threaded and multi-threaded tests must be pinned differently: one
 * big core for the first, a 4-core group for the second. */
static int want(const char *only, const char *name) {
    if (only == NULL) return 1;
    size_t n = strlen(name);
    for (const char *p = only; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int only_quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);
    const char *only = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;

    test_t tests[] = {
        {"alu", t_alu, 0, 0},
        {"branch", t_branch, 0, 0},
        {"fp_double", t_fp_double, 0, 0},
        {"fp_x87", t_fp_x87, 0, 0},
        {"memcpy", t_memcpy, 0, 0},
        {"memloop", t_memloop, 0, 0},
        {"syscall", t_syscall, 0, 0},
        {"atomics", t_atomics, 0, 0},
        {"threads", t_threads, 0, 0},
    };
    const int n = (int)(sizeof(tests) / sizeof(tests[0]));

    printf("# fexbench arch=%s ptr=%zu filter=%s\n", ARCH_NAME, sizeof(void *),
           only == NULL ? "all" : only);
    double total = 0;
    for (int i = 0; i < n; i++) {
        if (!want(only, tests[i].name)) continue;
        double t0 = now_ms();
        uint64_t sum = tests[i].fn();
        double ms = now_ms() - t0;
        tests[i].ms = ms;
        tests[i].sum = sum;
        total += ms;
        printf("%-10s %9.1f ms  sum=%llu\n", tests[i].name, ms,
               (unsigned long long)sum);
    }
    printf("TOTAL      %9.1f ms\n", total);
    if (only_quick) return 0;
    /* one line, easy to grep into a results table */
    printf("CSV %s", ARCH_NAME);
    for (int i = 0; i < n; i++) {
        if (!want(only, tests[i].name)) continue;
        printf(",%.1f,%llu", tests[i].ms, (unsigned long long)tests[i].sum);
    }
    printf(",%.1f\n", total);
    return 0;
}
