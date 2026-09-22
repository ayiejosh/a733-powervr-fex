# Open PowerVR driver on the A733 — build spike (stages 1–2 done, stage 3 staged)

Goal: run the **mainline** `powervr` driver on this board instead of the vendor `pvrsrvkm` DDK,
because the vendor DDK is what blocks Wayland (no surface extensions) and very likely what KWin's
Mesa kopper failure is downstream of.

Everything here builds **out-of-tree against the installed 6.6 headers** — no kernel rebuild, no
bootloader change, nothing risky. Source versions: v6.8 (the first release with both the driver and
the `drm_gpuvm` API this driver wants).

## Stage 1 — drm_gpuvm backport: DONE, loads

`shim/` is a v6.8 `drm_gpuvm.c` + `drm_gpuvm.h` with three adaptations:

| adaptation | why |
|---|---|
| two iteration macros in `gpuvm_compat.h` (`drm_gem_for_each_gpuvm_bo[_safe]`) | they live in v6.8's `drm_gem.h`, absent in 6.6 (`obj->gpuva` itself *does* exist) |
| `drm_exec_init(a, b, 0)` → `drm_exec_init(a, b)` | this kernel's `drm_exec` is the v6.7-era 2-arg form (`CONFIG_DRM_EXEC=m`) |
| 12 `drm_gpuva_*` exports renamed to `ks_drm_gpuva_*` | the kernel **already exports those names** with 6.6 signatures (older GPUVA manager); modpost refuses duplicates and binding to them would be a struct-layout mismatch |
| `SZ_128G` defined | added to `linux/sizes.h` after 6.6 |

Result: `drm_gpuvm.ko` (46 KB) builds and loads; exports 26 `drm_gpuvm_*` + 12 `ks_drm_gpuva_*`.
**Load order matters: `modprobe drm_exec` first**, then `insmod drm_gpuvm.ko`.

## Stage 2 — the driver: BUILDS

`powervr.ko`, 272 952 B, vermagic matches, `0` unresolved symbols, and it carries an `of:` alias for
`img,gpu` so it can bind the vendor DT node. Three adaptations, both patches in `driver/`:

1. **scheduler drift (the real one).** v6.8's driver wants `drm_sched_job_init(..., credits, ...)`
   and `drm_sched_init(..., submit_wq, num_rqs, ...)`; this 6.6 kernel has neither. Both call sites
   in `pvr_queue.c` were adapted to the 6.6 signatures. **This degrades the driver's queue
   arbitration** — acceptable for a bring-up, not shippable. Everything else the driver uses from
   `drm_sched` matches 6.6 exactly.
2. `img,gpu` added to the `dt_match` table (vendor node uses a bare compatible).
3. include paths for the in-tree UAPI header + our local `drm/drm_gpuvm.h`.

## Stage 3 — bind test: staged, not run

`stage3.sh` does: `modprobe drm_exec` → `insmod drm_gpuvm.ko` → `insmod powervr.ko` → report.
Acceptance: `[drm] Initialized powervr … for 1800000.gpu` in the kernel log and a DRM node named
`powervr`.

It **aborts if `pvrsrvkm` is loaded** (it owns the GPU node), so it needs:

```sh
sudo systemctl disable pvrsrvkm-load.service && sudo reboot   # GPU unclaimed
sudo /home/radxa/kspike/stage3.sh
# revert:
sudo rmmod powervr; sudo rmmod drm_gpuvm; sudo modprobe pvrsrvkm
sudo systemctl enable pvrsrvkm-load.service
```

Known risks: `panic_on_oops=1`, so a bad probe is a reboot (watchdog recovers, ~96 s); the vendor DT
node may lack the clocks/power-domains the open driver expects — that would show as a clean probe
failure, not a crash, and would mean a DT overlay is the next step.

## Stage 4 (not started)

Userspace: Mesa with `-Dvulkan-drivers=imagination` — Debian ships no pvr Vulkan ICD, and our BVNC
entry exists only in Mesa main. Then the payoff: Wayland and possibly KWin compositing.
