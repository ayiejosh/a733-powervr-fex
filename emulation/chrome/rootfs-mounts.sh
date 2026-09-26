#!/bin/bash
# rootfs-mounts.sh — give the x86-64 guest the kernel filesystems it assumes exist.
#
# Why this exists (2026-09-22). The Chrome launchers export FEX_ROOTFS and exec Chrome
# directly. They never go through crd-run.sh / crd-rootfs-enter.sh, which are the only
# scripts that bind-mount the kernel filesystems into the rootfs. So the guest's /proc
# and /dev/shm are empty directories, and two things break:
#
#  1. /proc is empty -> a guest `open("/proc")` returns an fd on <rootfs>/proc, and any
#     relative lookup against it fails with ENOENT:
#         fstatat(open("/proc"), "self/task/") == -1 ENOENT      (always, 200/200)
#     while the absolute equivalent works, because FEX special-cases absolute /proc
#     paths. Chrome calls exactly that at sandbox/linux/services/thread_helpers.cc:41:
#         fstatat(proc_fd, "self/task/", &task_stat, 0); PCHECK(0 == fstatat_ret);
#     The PCHECK fires, the process dies with "Check failed: . : No such file or
#     directory (2)", and the browser then waits forever on IPC for the child that
#     never connects. That is the "Chrome hangs on this board" symptom.
#  2. /dev/shm is empty/absent -> Chrome cannot use POSIX shared memory for its
#     renderer<->browser frame transport, which is why every launcher carries
#     --disable-dev-shm-usage (send frames through files on disk instead).
#
# Mounts are --rbind + --make-rslave, matching crd-rootfs-enter.sh. The rslave step is
# not optional: a recursive /sys bind once propagated an unmount back and killed the
# host's /sys/fs/cgroup.
#
# usage: sudo ./rootfs-mounts.sh [--status|--unmount]
set -u
ROOT=${ROOTFS:-/home/radxa/crd-rootfs}
MOUNTS="proc sys dev dev/pts"

status() {
  for m in $MOUNTS; do
    if mountpoint -q "$ROOT/$m"; then
      printf '  %-10s mounted    (%s)\n' "/$m" "$(findmnt -no SOURCE,FSTYPE "$ROOT/$m" 2>/dev/null)"
    else
      printf '  %-10s NOT MOUNTED -- guest sees %s entries\n' "/$m" "$(ls -A "$ROOT/$m" 2>/dev/null | wc -l)"
    fi
  done
}

if [ "${1:-}" = --status ]; then status; exit 0; fi

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

if [ "${1:-}" = --unmount ]; then
  for m in dev/pts dev sys proc; do
    mountpoint -q "$ROOT/$m" && umount -R "$ROOT/$m" 2>/dev/null && echo "unmounted /$m"
  done
  status; exit 0
fi

for m in $MOUNTS; do
  mkdir -p "$ROOT/$m"
  if mountpoint -q "$ROOT/$m"; then
    echo "  /$m already mounted, leaving it alone"
    continue
  fi
  if mount --rbind "/$m" "$ROOT/$m" 2>/dev/null; then
    # CRITICAL: never let a guest unmount propagate back to the host.
    mount --make-rslave "$ROOT/$m" 2>/dev/null || true
    echo "  /$m <- rbind of host /$m (rslave)"
  else
    echo "  /$m FAILED to bind mount" >&2
  fi
done

echo
status
echo
echo "guest /dev/shm: $(df -h "$ROOT/dev/shm" 2>/dev/null | tail -1 | awk '{print $2" total, "$4" avail"}')"
