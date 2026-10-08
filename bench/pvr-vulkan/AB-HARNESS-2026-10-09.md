# ab.sh — the full A/B in one run, and the two bugs that were blocking it

## What it does

```
./ab.sh [probe:size:count ...]      # default: the standard matrix
```

Stops the desktop (SDDM), runs **arm open fully**, switches, runs **arm vendor fully**, restarts the desktop
**via a trap so it returns even if interrupted**, then prints the diff from the recorded runs.

## Result — the first complete A/B

| probe | open | vendor | ratio |
|---|---|---|---|
| `cstp` (no loop) | 284.9 M inv/s | 369.9 | **1.30×** |
| `cstpf` (float loop) | 87.0 | 145.5 | **1.67×** |
| `cstpi` (int loop) | 72.7 | 146.8 | **2.02×** |
| `vkheavy` 2048 | 255.242 ms | 180.065 ms | **1.42×** |
| `vkrender` 2048 | 13.772 ms | 7.260 ms | **1.90×** |
| `vkrender` 512 | 1.502 ms | 0.732 ms | **2.05×** |

**The open values match the earlier hand-measurements exactly** (cstpi 72.7, cstpf 87.0, vkheavy 255.24,
vkrender 13.77), so the harness is consistent with the methods used before it existed.

## Bug 1 — `insmod` in `switch-open.sh` (this broke the open driver entirely)

```
powervr: Unknown symbol drm_gem_shmem_get_pages_sgt (err -2)
powervr: Unknown symbol drm_sched_job_arm (err -2)
...
```

**`insmod` does not resolve module dependencies**, so `powervr.ko` could not link against `gpu_sched`,
`drm_shmem_helper` and `drm_exec`. **The running kernel also changed across the reboot (the module had to be
rebuilt for `6.6.98-5-aw2511`).** Fixed by:

* rebuilding the module against the running kernel's headers, and
* replacing `insmod /home/radxa/kernel-src/powervr/powervr.ko` with **`modprobe powervr`**, after installing
  the module into `/lib/modules/$(uname -r)/extra/powervr/` and running `depmod -a`.

**Verified: `vkrender` 512 PASS under `powervr`, and the bound driver reads back as `powervr`.**

## Bug 2 — driver detection in `harness.py`

`os.path.realpath()` on `/sys/bus/platform/devices/1800000.gpu/driver` returns **the device path itself**
when no driver is bound, so `basename` gave `driver` and **the bound driver was mislabelled** — which is why
the first A/B showed "bound driver: driver" and produced an empty open column. Fixed by reading the symlink
with `os.readlink()` instead.

**Both bugs produced silent wrong answers, not errors** — the first A/B looked like it had run, and its diff
showed only vendor numbers with no indication that the open arm had failed. **The log now carries the bound
driver per record, which is what made it visible.**
