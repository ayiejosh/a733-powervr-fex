# Local patches that make the mainline PowerVR driver work on the A733

These are diffs from the v6.8 `drivers/gpu/drm/imagination` sources to the tree that
actually builds and runs on this 6.6 BSP kernel (`/home/radxa/kspike/img`). They are
bring-up work, not upstream proposals: the right upstream shape for most of this is a
device-tree overlay plus real clock/power-domain modelling.

| patch | what it does |
|---|---|
| `pvr_drv-img-gpu-match.patch` | matches the vendor device tree's `img,gpu` on the GPU node |
| `pvr_device-clock-bringup.patch` | maps the vendor node's `clk` onto the driver's `core` clock |
| `pvr_power-ks-bringup.patch` | enables `clk_bus` and deasserts `reset_bus` around power-up; **acquires clock and reset once** and caches them (see below) |
| `pvr_device-ks-bringup.patch` | the `ks_bus_clk` / `ks_bus_rst` / `ks_bringup_done` state that the previous patch needs |
| `pvr_queue-6.6-scheduler-adaptation.patch` | adapts the driver's queue code to this kernel's `drm_sched`, which lacks `credits`, `submit_wq` and `num_rqs` (this is option (b); option (a) is to port those instead) |

## The bug worth remembering

The `clk_bus` / `reset_bus` handling first lived inline in `pvr_power_device_resume()`,
which runs on *every* runtime-PM resume. That meant a `devm_clk_get()` and a
`devm_reset_control_get_optional_exclusive()` per resume: a devres leak each time, and
because the reset is exclusive, the second attempt failed with `-EBUSY`, which produced a
kernel warning from `pvr_power_device_resume` and a `ks-bringup: no reset_bus (-16)` line.
The state is now acquired once and cached in `struct pvr_device`; only the clock *enable*
stays per-resume, because the mainline driver disables its clocks when it suspends.

## Building

```sh
cd /home/radxa/kspike/img
sudo make -C /lib/modules/$(uname -r)/build M=$PWD \
     KBUILD_EXTRA_SYMBOLS=/home/radxa/kspike/mod/Module.symvers modules
```

`KBUILD_EXTRA_SYMBOLS` is required: without it modpost fails on the `drm_gpuvm` symbols
provided by the backported `drm_gpuvm.ko` in `../mod`.
