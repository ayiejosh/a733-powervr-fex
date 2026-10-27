# Diagnostics used by this project

All board-side probes are read-only unless noted; run them only while the corresponding driver stack is
loaded, and restore the board (`open-perf/open-exit.sh`) afterwards.

## tools/gpu-regs/ - GPU MMIO (requires root, /dev/mem)
- `mtsread.c`, `mtsdump.c`, `regrw.c`, `watchpoke.c` - read/poke individual MTS/contiguous registers.
- `kicksnap.c` - snapshot 0x000-0x4ff right after the kick-pending bit (0xd20) goes 1.
- `kicktl.c` - timeline of GPU MMIO 0x000-0x1000 at ~tens of microseconds per sample, starting at the kick
  (writes a small binary: header + timestamps + register dwords).
- `regdump.c` - page-wise robust 64 KiB register dump (catches SIGBUS on unclocked pages).
- `stoponkick.c` - freeze a PID the microsecond its kick goes pending (lets us dump the still-live firmware
  context).

## tools/kernel/ - open-kernel patches used for evidence
- `pvr_fwva-debugfs.patch` - debugfs `pvr_dbg/fwva` that reads firmware MIPS VA via the firmware page table
  and `memremap` (used for FWCtx/RTData dumps and page-table summaries).
- `pvr_rogue_fwif.h` trace-ring enlargement was a temporary local edit (12000 -> 200000 dwords); the patch
  is not carried here - re-apply by editing `ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS` and rebuilding the
  module if full-mask traces are needed again.

## tools/verify/
- `verify.sh` - one-command read-only health gate (driver binding, firmware sha256, kernel faults, shim probe
  ALL PASS, published-firmware watch, desktop state).
- `measure_cpu.py` / `measure.py` - run a workload under a given Vulkan ICD and report wall vs child CPU time.
- `syncobj_chain_shim.c` - LD_PRELOAD shim that histograms DRM syncobj call-site chains (used to localise the
  per-frame syncobj churn in Mesa's pvr_arch_queue.c).

## tools/firmware/
- `check-open-firmware.sh` - poll linux-firmware's powervr tree for a genuine open-ABI firmware for this
  board's BVNC; prints install/test steps if one appears.
- `make-img-bundle.sh` - build `img-evidence-bundle.tar.gz` for the IMG request.
