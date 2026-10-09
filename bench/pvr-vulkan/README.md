# pvr-vulkan — does this board's PowerVR actually execute Vulkan work?

The A733 has a PowerVR B-Series **BXM-4-64 MC1** (BVNC 36.56.104.183) with **no
usable desktop GL**, which is why desktop GL here goes through zink. Vulkan is a
different question, and it turned out to have three separate answers depending on
which driver and which ICD you mean. These tools measure them.

## Files

| file | what it answers |
|---|---|
| `vktest.c` | can a Vulkan driver take a shader, dispatch it, and return the right bytes? Prints device/queue/memory facts, then verifies every element of a 1M-element xorshift and reports throughput |
| `compute.comp` | the compute shader (xorshift per element, so the host can predict every output exactly) |
| `build.sh` | glslangValidator → SPIR-V → embedded as `uint32_t` words → `vktest` |
| `drmdevs.c` | inventory of DRM devices as **Mesa sees them**: node paths, bus type, DT `compatible` list, and the `drmGetVersion` name/version that pvr_winsys_create switches on |
| `compare-icds.sh` | runs `vktest` through each ICD installed/built here and prints one table |

## Build and run

```sh
./build.sh                     # needs glslangValidator, gcc, python3
./drmdevs                      # what libdrm reports for card0/card1/renderD128
./compare-icds.sh 20 1048576   # 20 dispatches x 1M elements through each ICD
```

`VKTEST_API` (default `1.0`) sets the requested `apiVersion`. This matters: the
loader fails `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER` when the app
asks for more than the ICD's JSON declares, and Mesa 25.0.7's pvr ICD declares
only 1.0.

## The three answers

**1. Vendor ICD — works.** `/usr/lib/libVK_IMG.so` (Imagination DDK
`24.2@6603887`, Vulkan 1.3.277, registered by
`/usr/share/vulkan/icd.d/img_icd.json`) talks to the vendor `pvrsrvkm` module.
Measured, 1M elements per dispatch:

```
submit+wait: 24.971 ms total, 2.497 ms/dispatch
throughput:  3.36 GB/s (read+write)
RESULT: PASS - 1048576/1048576 elements correct
```

**2. Mesa pvr against the vendor module — closed by design.** Mesa's
`pvrsrvkm` ("srv") winsys compiles (`-Dimagination-srv=true`) and the kernel node
is even named `pvr`, but `pvr_srv_winsys_create()` calls
`pvr_is_driver_compatible()`, which accepts **only downstream driver version
1.17** (`PVR_SRV_VERSION_MAJ/MIN`). This board's vendor module reports
`24.2.6603887`, so it returns `VK_ERROR_INCOMPATIBLE_DRIVER` before any ioctl.

**3. Mesa pvr against the mainline `powervr` driver — works.** This is the path
that needed real work, and it now executes GPU work end to end:

```
WARNING: powervr is not a conformant Vulkan implementation, testing use only.
device[0] name="PowerVR B-Series BXM-4-64 MC1" api=1.2.328 vendor=0x1010 device=0x36104183
queue family 0: flags=0x7 count=2
memory: type 0, heap 0 (4436 MB)
dispatching 10 x 1048576 elements (4 MiB per dispatch)...
submit+wait: 28.797 ms total, 2.880 ms/dispatch
throughput:  2.91 GB/s (read+write)
RESULT: PASS - 1048576/1048576 elements correct
```

Three gates had to be cleared, in this order, and each one is a distinct fact:

1. **Enumeration is a DT `compatible` whitelist** (`pvr_drm_configs[]`): it wants
   `img,gpu` for the render node and `allwinner,sunxi-drm` for the display node,
   and without a match the driver enumerates **zero** devices. See
   `mesa/0001-pvr-add-A733-img-gpu-platform.patch`.
2. **Device info for the BVNC**: 25.0.7–25.2 ship only `axe-1-16m`, `bxs-4-64`,
   `gx6250`, so `pvr_device_info_init()` returns `-ENODEV` → the device is
   rejected. Mesa 25.3.0 ships `bxm-4-64.h` with
   `PVR_DEVICE_IDENT_36_V_104_183`.
3. **A working shader compiler**: 25.0.7's `pco_nir.c` is a stub (four
   `finishme`s) and it falls back to hard-coded programs it does not have for
   this GPU, so `vkCreateComputePipelines` fails even with the device info
   backported. 25.3.0 compiles at runtime. Backporting device info into 25.0.7 is
   therefore not a shortcut; `mesa/0002-...patch` exists to document why.

25.3.0 in turn requires CLC → LLVM + LLVMSPIRVLib 19.1.x + libclc + SPIRV-Tools;
`mesa/README.md` has the recipe and the two things this board was missing.

## Notes

- The kernel side already reports the right BVNC: `pvr_dev_query_gpu_info_get()`
  fills `gpu_id` from `pvr_gpu_id_to_packed_bvnc(&pvr_dev->gpu_id)`, and that
  struct is filled by `pvr_load_gpu_id()` reading the hardware control registers.
  Mesa's vendored `pvr_drm.h` is byte-identical to this kernel's UAPI header.
- Taking the GPU away from the vendor module needs the desktop stopped
  (`systemctl stop display-manager`), which drops `pvrsrvkm` refs 188 → 0 in
  under a second. The session running this work lives in
  `user@1000.service/app.slice/dsh-web.service`, not the graphical session, so it
  survives. See `kernel/open-driver-spike/stage4-mainline-vulkan.sh`.

---

# Session additions (2026-10-09)

The sections above are the original bench documentation and still accurate. **What follows was added during the
benchmarking session** - the harness, the A/B tooling, the safety rules, and the corrected findings.

## The tools built

| tool | what it does |
|---|---|
| `harness.py <probe> <size> <count> [ENV=V …] [--driver=open\|vendor]` | **one run → driver, speed (min/median/max + spread), correctness, per-stage job durations, critical path, CPU split, bandwidth.** Appends to `harness-log.jsonl`. **Its `--driver=` checks for kwin and refuses to switch if a desktop is live.** |
| `sweep.sh` | the whole probe matrix → one consolidated table |
| `ab.sh` | **full A/B in one run** — closes the desktop, arm open fully, arm vendor fully, reopens via trap, prints the diff |
| `components.sh` | 30 component checks — modules, vermagic vs running kernel, driver binding, firmware (+md5), ICDs **and whether each named library resolves**, tracepoints, guard |
| `enumvk.c` | Vulkan extension/feature enumerator |

## Headline findings

* **~2.4x render gap** (2.39x median, **1.94x worst case**) — measured with ranges on both sides.
* **The four PCO fixes are PROBE-LEVEL: 2.7–3.4x on loop-bound compute and NO measurable client effect** on two
  scenes. **Do not describe them as a client-level win.**
* **Kernel share of the client frame: 62.5%**, independently measured.
* **Stage shapes differ:** geometry is **flat** (open wins, 0.43x); PR and fragment are **tile-bound**, so the
  PR's 4.03x is a *work-shape* problem (Mesa's) and the fragment's 2.17x is a *raster-cost* problem (outside Mesa).

## Safety - read before switching drivers

**The vendor `pvrsrvkm` driver has crashed this board twice**: its rewrapped firmware faulting, and a NULL-deref
in its **file-close path** during a driver-switch sequence. **Both required a reboot; the guard recovered
automatically both times.**

* **Never unbind while kwin/X is alive** — the guard and `harness.py` both refuse.
* **Batch switch operations.** Repeated weston teardown under the vendor driver is the exposure.
* **Prefer `./ab.sh`** for two-arm work: one controlled switch pair with a trap-restore.

## Measurement discipline

* **throughput / kernel timestamps / counts** — ~1% repeatable, insensitive to host load. **Use these.**
* **per-stage job durations** — 0.7–4.4% repeatable. **The decomposition's foundation.**
* **wall-clock FPS** — ~0.2 ms fixed jitter, so percentage spread scales inversely with frame time
  (**4.5% at 2048, 27% at 512**). **Never a 3-run median at small sizes.**
* **Always check the tool measured what it claims** — two of this session's errors were instrumentation
  silently not measuring (a mislabelled driver, a missing client process).

**For the full record and the corrections, see `FINAL-HANDOVER-2026-10-08.md` - and read its top banner.**

