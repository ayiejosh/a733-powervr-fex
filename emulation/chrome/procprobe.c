// procprobe.c — minimal reproducer for the Chrome launch failure on this board.
//
// Chrome dies at sandbox/linux/services/thread_helpers.cc:41:
//     int fstatat_ret = fstatat(proc_fd, "self/task/", &task_stat, 0);
//     PCHECK(0 == fstatat_ret);   // "Check failed: . : No such file or directory (2)"
// where proc_fd = open("/proc", O_RDONLY|O_DIRECTORY).
//
// v1 established this is deterministic (300/300), not a race, and that the absolute
// form works. v2 narrows the mechanism: is it /proc-specific, or does FEX fail every
// dirfd-relative lookup? Each variant is probed N times and counted.
//
// build:  x86_64-linux-gnu-gcc -O1 -static -o procprobe procprobe.c
// run:    ./procprobe [iterations]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NVARIANTS 8
static const char *names[NVARIANTS] = {
  "1 fstatat(open(/proc)          , \"self/task/\")   <- the Chrome call",
  "2 fstatat(open(/proc)          , \"self\")",
  "3 fstatat(open(/etc)           , \"hostname\")     (non-/proc dir)",
  "4 fstatat(open(/tmp)           , \".\")            (non-/proc dir)",
  "5 fstatat(open(/)              , \"proc/self/task/\")",
  "6 fstatat(open(/proc/self)     , \"task/\")",
  "7 stat  (\"/proc/self/task/\")  absolute           (control)",
  "8 fstatat(openat(/proc)        , \"self/task/\")    (openat dirfd)",
};
static long fails[NVARIANTS], ran[NVARIANTS];

static int dirfd_of(const char *p, int flags) {
  errno = 0;
  return open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | flags);
}

static void probe_all(int iter) {
  struct stat st;
  int fd;

  fd = dirfd_of("/proc", 0);
  if (fd >= 0) { ran[0]++; if (fstatat(fd, "self/task/", &st, 0)) fails[0]++; close(fd); }
  fd = dirfd_of("/proc", 0);
  if (fd >= 0) { ran[1]++; if (fstatat(fd, "self", &st, 0)) fails[1]++; close(fd); }
  fd = dirfd_of("/etc", 0);
  if (fd >= 0) { ran[2]++; if (fstatat(fd, "hostname", &st, 0)) fails[2]++; close(fd); }
  fd = dirfd_of("/tmp", 0);
  if (fd >= 0) { ran[3]++; if (fstatat(fd, ".", &st, 0)) fails[3]++; close(fd); }
  fd = dirfd_of("/", 0);
  if (fd >= 0) { ran[4]++; if (fstatat(fd, "proc/self/task/", &st, 0)) fails[4]++; close(fd); }
  fd = dirfd_of("/proc/self", 0);
  if (fd >= 0) { ran[5]++; if (fstatat(fd, "task/", &st, 0)) fails[5]++; close(fd); }
  ran[6]++; if (stat("/proc/self/task/", &st)) fails[6]++;
  errno = 0; fd = openat(AT_FDCWD, "/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) { ran[7]++; if (fstatat(fd, "self/task/", &st, 0)) fails[7]++; close(fd); }

  (void)iter;
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 200;
  for (int i = 0; i < n; i++) probe_all(i);
  printf("\n  %-66s %s\n", "variant", "failed/ran");
  for (int i = 0; i < NVARIANTS; i++)
    printf("  %-66s %ld/%ld%s\n", names[i], fails[i], ran[i],
           fails[i] == ran[i] && ran[i] ? "   <== ALWAYS FAILS" : (fails[i] ? "   <== flaky" : ""));
  long total = 0;
  for (int i = 0; i < NVARIANTS; i++) total += fails[i];
  return total ? 1 : 0;
}
