/*
 * tsolitmus — does running with TSO emulation OFF actually break x86 ordering?
 *
 * The board's FEX config has TSOEnabled:"0", which upstream describes as
 * "highly likely to break any multithreaded application if disabled" while also
 * being the single biggest speed knob. That is a risk claim nobody had measured
 * here, so this measures it.
 *
 * WHY THIS SHAPE
 * --------------
 * A handshake-based litmus test (writer sets x then y, reader checks y implies
 * x) deadlocks unless the two threads agree on round boundaries, and it needs
 * program-order guarantees from the very memory model under test. This version
 * has no handshake at all:
 *
 *   writer:  for each slot i and pass p:  a = v; then b = v;   (plain stores)
 *            where v = p*SLOTS + i, a value that is never reused.
 *   reader:  repeatedly read b, then a, then b again. If both b reads agree and
 *            a differs, a store pair was observed out of order.
 *
 * Because v is unique, seeing b == v means the writer's pair for that v is the
 * only possible source, and x86 TSO guarantees the a store was already visible.
 * The double b read rules out the innocent case where the reader simply raced a
 * later update of the same slot.
 *
 * A reported violation therefore means the guest observed two plain stores out
 * of order — exactly the silent corruption upstream warns about: lock-free x86-64
 * code that assumes TSO (most of it) has no fence where it needs one.
 *
 * IMPORTANT: zero violations is NOT proof of safety. This is a detector, not a
 * verifier; a rare reordering can hide from any finite run. What it can do is
 * falsify: one violation is enough to condemn the setting.
 *
 * Build: gcc -O2 -static -pthread -o tsolitmus_arm64 tsolitmus.c
 *        x86_64-linux-gnu-gcc -O2 -static -pthread -o tsolitmus_x64 tsolitmus.c
 * Run:   taskset -c 6,7 ./tsolitmus_arm64 [passes]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SLOTS 32768

/*
 * a and b are deliberately placed a cache line apart. Adjacent fields live in
 * the same line, and a core's stores to one line drain in order, so a
 * same-line pair cannot show the reordering this test is looking for -- the
 * first version of this file did exactly that and reported zero violations even
 * on native weakly-ordered ARM64, which made it useless as a detector.
 */
static struct {
    volatile unsigned a;
    char pad[64];
    volatile unsigned b;
    char pad2[60];
} buf[SLOTS];

static volatile int writer_done;
static long passes = 30;
static unsigned long samples, violations, reordered;

static void *writer(void *arg) {
    (void)arg;
    for (long p = 1; p <= passes; p++) {
        for (long i = 0; i < SLOTS; i++) {
            unsigned v = (unsigned)(p * SLOTS + i);
            buf[i].a = v;
            __asm__ __volatile__("" ::: "memory"); /* program order for the compiler */
            buf[i].b = v;
        }
    }
    writer_done = 1;
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    while (!writer_done) {
        for (long i = 0; i < SLOTS; i++) {
            unsigned b1 = buf[i].b;
            if (b1 == 0) continue;
            unsigned a = buf[i].a;
            unsigned b2 = buf[i].b;
            if (b1 != b2) continue; /* the writer moved on mid-read: discard */
            samples++;
            if (a != b1) {
                violations++;
                if (a < b1) reordered++; /* saw the second store before the first */
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) passes = atol(argv[1]);
    pthread_t w, r;
    pthread_create(&w, NULL, writer, NULL);
    pthread_create(&r, NULL, reader, NULL);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    printf("samples=%lu violations=%lu reordered=%lu\n", samples, violations, reordered);
    return violations == 0 ? 0 : 1;
}
