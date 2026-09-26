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

---

## STAGE 3: **PASSED** — the open driver runs on this board

```
powervr 1800000.gpu: ks-bringup: clk_bus enabled
powervr 1800000.gpu: ks-bringup: reset_bus deasserted
powervr 1800000.gpu: [drm] loaded firmware powervr/rogue_36.56.104.183_v1.fw
powervr 1800000.gpu: [drm] FW version v1.1 (build 6976702 OS)
[drm] Initialized powervr 1.0.0 20230904 for 1800000.gpu on minor 1
```

Kernel log clean, no crash. That is the mainline driver, our backported `drm_gpuvm`, the v6.8 driver
adapted to this 6.6 scheduler, the open-ABI firmware for our exact BVNC — and **no vendor DDK**.

### What actually had to be fixed to get there (in order)

1. **module names/deps.** `drm_exec`, `drm_shmem_helper` are modules, and the scheduler module is
   named **`gpu-sched`**, not `drm_sched` (`modprobe drm_sched` says "not found" even though
   `CONFIG_DRM_SCHED=m`). Load order: `drm_exec gpu-sched drm_shmem_helper drm_gpuvm powervr`.
2. **clock name mapping.** The vendor node names its clocks `clk_parent clk clk_bus clk_800 …`; the
   mainline driver asks for `core`. Falling back to the *unnamed* clock picks `clk_parent` — the GPU
   stays dark and **the BVNC reads as 0.0.0.0**, which makes the driver request
   `rogue_0.0.0.0_v1.fw`. Mapping `core -> clk` fixes it.
3. **bus clock + reset.** The vendor node has a separate `clk_bus` and a `reset_bus` reset that the
   mainline driver knows nothing about. Until both are handled the control registers read zero.
   `pvr_power-busclock-reset.patch` enables/deasserts them.
4. **(for the whole build)** the `drm_gpuvm` backport, the 6.6 `drm_sched` call-site adaptation, the
   `img,gpu` match entry, the UAPI include paths.

### What this does and does not prove

Proves: the driver binds, the DT glue works, the firmware is accepted (BVNC matched — `pvr_fw_validate()`
compares the packed BVNC against the hardware), and the GPUVA manager backport is functional.

Does **not** prove rendering: no userspace has talked to the driver yet. That needs Mesa built with
`-Dvulkan-drivers=imagination` (stage 4) — Debian ships no pvr Vulkan ICD. Only after that can
option (a) (a real multi-ring `drm_sched` port) be compared against option (b) under load; (b) has
now been shown *sufficient to bind*, which is the first half of that answer.

### Operational note

The vendor Xorg cannot start without the vendor GPU module (it dies in glamor init with no software
fallback), so any test that frees the GPU node takes the desktop down for its duration. Recovery:
remove `/etc/modprobe.d/blacklist-pvrsrvkm.conf`, re-enable `pvrsrvkm-load.service`, reboot.
