// unlinkprobe.c — does FEX break dirfd-relative unlink?
//
// While purging google-chrome-stable inside the guest rootfs, dpkg removed the whole
// payload correctly and then failed with:
//     unable to delete control info file
//     '/var/lib/dpkg/info/google-chrome-stable.postinst': No such file or directory
// but that file was still present afterwards (20367 bytes). So the unlink itself failed
// with ENOENT on an existing file.
//
// That is the same shape as the /proc bug (procprobe.c): absolute paths fine, a path
// resolved relative to a directory fd not fine. If unlinkat() with a dirfd is broken,
// every dpkg/apt operation in that rootfs is at risk, which is worth knowing before
// anyone runs apt inside it.
//
// build:  x86_64-linux-gnu-gcc -O1 -static -o unlinkprobe unlinkprobe.c
// run:    ./unlinkprobe
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void rm(const char *p) { unlink(p); }

static void report(const char *name, int rc, int err) {
  int exists = (access(name, F_OK) == 0);
  printf("  %-52s rc=%-3d errno=%-2d %-22s still_exists=%s\n",
         name, rc, rc ? err : 0, rc ? strerror(err) : "ok", exists ? "YES" : "no");
}

int main(void) {
  char base[] = "/tmp/unlinkprobe.XXXXXX";
  if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
  char path[512], dir[512];

  printf("\n  variant%s\n", "");

  /* 1. absolute unlink */
  snprintf(path, sizeof path, "%s/abs", base);
  int fd = open(path, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
  errno = 0; int r = unlink(path); int e = errno;
  report("1 unlink(abs path)", r, e); if (r) fails++;

  /* 2. dirfd-relative unlinkat — the dpkg-shaped call */
  snprintf(dir, sizeof dir, "%s/d", base);
  mkdir(dir, 0755);
  snprintf(path, sizeof path, "%s/d/rel", base);
  fd = open(path, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
  int dfd = open(dir, O_RDONLY | O_DIRECTORY);
  errno = 0; r = (dfd < 0) ? -1 : unlinkat(dfd, "rel", 0); e = errno;
  report("2 unlinkat(dirfd, \"rel\")           <- dpkg-shaped", r, e); if (r) fails++;
  if (dfd >= 0) close(dfd);

  /* 3. unlinkat with AT_FDCWD and an absolute path */
  snprintf(path, sizeof path, "%s/abs2", base);
  fd = open(path, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
  errno = 0; r = unlinkat(AT_FDCWD, path, 0); e = errno;
  report("3 unlinkat(AT_FDCWD, abs path)", r, e); if (r) fails++;

  /* 4. dirfd-relative unlinkat on a *nested* dirfd, like /var/lib/dpkg/info */
  snprintf(path, sizeof path, "%s/d/sub", base); mkdir(path, 0755);
  snprintf(path, sizeof path, "%s/d/sub/deep", base);
  fd = open(path, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
  snprintf(dir, sizeof dir, "%s/d/sub", base);
  dfd = open(dir, O_RDONLY | O_DIRECTORY);
  errno = 0; r = (dfd < 0) ? -1 : unlinkat(dfd, "deep", 0); e = errno;
  report("4 unlinkat(nested dirfd, \"deep\")", r, e); if (r) fails++;
  if (dfd >= 0) close(dfd);

  /* 5. stat a file then unlink it, the exact dpkg sequence */
  snprintf(path, sizeof path, "%s/info", base); mkdir(path, 0755);
  snprintf(path, sizeof path, "%s/info/pkg.postinst", base);
  fd = open(path, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
  struct stat st; int sr = stat(path, &st);
  errno = 0; r = unlink(path); e = errno;
  report("5 stat-then-unlink(abs, mimics dpkg info file)", sr ? -1 : r, e);
  if (r) fails++;

  printf("\n  TOTAL FAILURES: %d\n\n", fails);
  return fails ? 1 : 0;
}
