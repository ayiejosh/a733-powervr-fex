# Xwayland profile: not a copy - futex 61.7%, ioctl 35.4%

## Method

`strace -f -c -p $(pgrep -x Xwayland)` for 8 s while a zink client ran at ~40 FPS, open stack,
weston + Xwayland, 640x480.

## Result

```
% time   seconds   calls  syscall
100.00   5.369571      66  total (80662)
 61.74   3.315270    4273  futex
 35.44   1.903099   67014  ioctl
  0.72   0.038755    4424  close
  0.16   0.008740     519  writev
  0.05   0.002926      88  sendmsg
```

**No `write`, no SHM, no memcpy-sized traffic.** The zero-copy path (DRI3 + Present, both in the
server's extension list) is already in use.

## The two terms

* **futex, 61.7%** - Xwayland is mostly blocked, not working. Largest single term; needs a
  futex-address trace to tell contention from ordinary waiting.
* **ioctl, 67,014 in 8 s = ~209/frame** - matches the independent count of ~190-250. 1.9 s of 8 s,
  so ~3.4 ms of Xwayland CPU per frame at a 25 ms frame (~13%).

## Consequence

The GEOM-only timeline migration removes ~1/5 of the syncobj create/destroy traffic, i.e. ~0.7
ms/frame - below noise. **"The migration had no measurable effect" is what this predicts**, rather
than proof the churn is irrelevant. Removing all of it is worth ~3.4 ms/frame (~13%), not the
~10 ms/frame originally assumed.

## What the 32x is not

Not the copy (there is none), and not the driver's job submission (raw render is only 2.5-4x down
and the KMS path already matches the vendor). The order of the remaining terms is futex, then ioctl,
then render.
