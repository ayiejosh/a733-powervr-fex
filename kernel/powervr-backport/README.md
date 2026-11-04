# Open `powervr` kernel driver, out-of-tree backport for the 6.6 `aw2511` kernel

The open stack runs the mainline `drm/imagination` driver (taken from linux-source-6.16), built as an
out-of-tree module against the Radxa `6.6.98-5-aw2511` kernel, which has no `CONFIG_DRM_POWERVR`.

## Source

- **Base:** `open-driver-perf/backup-2026-10-10/kernel-powervr-src.tgz`. This is the tree as
  installed on 2026-10-10: the 6.16 driver plus the 6.6 compatibility layer, chained fragment kicks
  (`chain_frag`), combined partial-render kicks (`combine_pr`), the cached firmware code, `pr_debug`
  on the hot path, and `drm_sched_start(..., true)` recovery.
- **Patches on top**, apply in order with `git am` (or `patch -p1`) in the extracted tree:
  - `0001-pvr_fw-add-os_config_flags-test-knob...patch`: a module parameter for the firmware's
    per-OS config flags (test knob, default unchanged).
  - `0002-pvr_queue-check-paired-and-chained-fragment-commands...patch`: **required.** It fixes a
    fragment CCCB overflow that wedged the GPU under Chromium (see below).
  - `0003`-`0007`: **required.** Five mainline `drm/imagination` MMU/VM fixes (all `Cc: stable`), in
    mainline order: `17e2030f3760` (VM lock before mapping), `7b824c293a6b` (map failures
    propagated), `0a8224058a58` (page-table count from the device address), `789f290b2e81`
    (preallocation sized by device address), `196d4d10c1b3` (remap references). In `0005` and `0006`
    one hunk was applied by hand: this tree allocates with `kzalloc()`, mainline with
    `kzalloc_obj()`. They fix a kernel oops reachable from any GPU client (see below).

## Build and install

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo cp /lib/modules/$(uname -r)/extra/powervr/powervr.ko{,.bak}   # keep the previous module
sudo cp powervr.ko /lib/modules/$(uname -r)/extra/powervr/powervr.ko
```

`open-driver-perf/tools/verify/switch-safe.sh open|vendor` switches the GPU between this driver
and the vendor `pvrsrvkm`, and reloads the module file on each switch.

## The CCCB fix (patch 0002)

**Symptom:** Chromium (ANGLE on zink on the open Vulkan driver) drawing WebGL on a 4K canvas made
the kernel print `WARNING ... pvr_cccb.c:178 pvr_cccb_write_command_with_header`. After that,
`powervr 1800000.gpu: Job timeout` repeated without end (about 21,800 times in 90 minutes). Later
PID 1 and systemd-logind sat in D state, and only a power cycle recovered the board.

**Cause:** a geometry job's paired partial-render job and its chained fragment job are written to
the **fragment** CCCB in the geometry job's `run_job()`. Their space, however, was checked in the
**geometry** CCCB (a nested `prepare_job()` with the geometry queue's entity) and one job at a
time. When the fragment queue was slow and the geometry queue fast, the fragment CCCB filled up
while the check kept passing. The write then failed, the command was dropped, and the kick was
still sent, so those done fences could never signal.

**Fix:**
- Size the whole fragment sequence (paired fences and command, plus the chained job) as one, and
  check it in the fragment CCCB. This covers mainline `4baf9e70cb75` and `9cd74f935306`.
- Wake on the size stored when the job registered.
- Let a second waiter on a CCCB wait behind the first instead of replacing it.
- Fix `combine_pr` at load, so the check and the write always agree.
- Do not chain a sequence that could never fit.

**Verified:** with this patch, 3 runs of the same Chromium 4K WebGL page, glmark2 (zink and native)
and a 15-minute glmark2 soak gave 0 job timeouts, 0 CCCB warnings and 0 kernel warnings.

## The VM_UNMAP oops (patches 0003-0007)

**Symptom:** Firefox (WebGL with `widget.dmabuf-webgl.enabled=false`) on the open stack oopsed the
kernel, twice in a row: `Unable to handle kernel NULL pointer dereference at virtual address
0000000000000008` in `pvr_mmu_op_context_unmap_curr_page()`, called from `pvr_ioctl_vm_unmap()`.
With `panic_on_oops=1` (this board's default) the board panicked and the watchdog rebooted it; with
it off, the client hung in `drm_gem_shmem_vm_close` and PID 1 went into D state.

**Cause:** the backport predates mainline's fixes for mappings whose page tables were never fully
built: a map that failed part-way still returned success, and the page-table preallocation was
counted from the offset into the buffer instead of the device address, so a mapping that crossed a
2 MiB (L0 table) boundary could run out of tables. A later unmap of that range followed a NULL L0
table pointer. Mainline `7b824c293a6b` carries the same oops in its commit message.

**Verified:** the same Firefox run that oopsed twice now completes all its phases, and the held
desktop sessions that followed (Firefox and Chromium runs, the app tests, the D3D11 tests) had 0
oopses and 0 GPU errors.
