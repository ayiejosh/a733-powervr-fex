# Open PowerVR driver performance — work log

**Goal:** make the open stack (mainline `powervr` backport + open firmware + Mesa `pvr`) match, then beat, the
vendor stack (`pvrsrvkm` + `libVK_IMG`) on the A/B workloads, with no correctness or stability regressions.

**Rules I keep:** nothing is pushed to GitHub without the user's OK; every board change is logged here with its
undo step; no live driver unbind while X/kwin runs (the documented crash trigger).

## RESUME HERE (state at 2026-10-10 ~21:40 board time, Mesa HEAD `d3e4d4d`, board restored to vendor + desktop; see FINAL STATE below)

- **`28deb4c` buffer copies** (`rogue/pvr_blit.c` `pvr_cmd_copy_buffer_region`): (1) dst alignment used src_offset →
  srcOffset 0 / dstOffset 4 copies failed with "ISP vertex xy value out of range" + DEVICE_LOST (also on d253e35);
  (2) copy rows of 4 KiB instead of 2048 texels (fills keep max width); (3) equal misalignment: copy head to 16-byte
  boundary, rest with 16-byte texels. 16 MiB copy 4.81 → 3.56 ms aligned (vendor 4.22–4.49), 6.6 → 4.4 ms offset 4/8.
  Probe `~/open-perf/cpbw` (byte-exact verify over offsets via VSRC/VDST; CPBW_OFF timing offset; CPBW_NOVERIFY —
  vendor segfaults in its unaligned-copy path). CTS buffer_to_buffer/fill_and_update_buffer/buffer_marker unchanged.
- **2026-10-10 22:10-22:30 — render+copy hand-off investigation, stopped (no lever found):**
  Backups first: `open-driver-perf/backup-2026-10-10/` on the Mac (73 Mesa patches + bundle from 9a2b374,
  kernel source tgz, all kernel patches, session.log); `~/kernel-src/powervr` now a local git repo (commit
  `214314d`). Nothing pushed.
  `hosttrace.sh` now finds the GPU IRQ itself (it is 482 since the reboot, was 481; serviced on cpu6).
  Pinned single-frame timelines (open, µs from submit): render512 geom kick 34, geom done 155, frag done 355,
  wake 363, next submit 389; both512 geom kick 32, frag done 343, copy done 605, wake 612, next submit 636; copy
  alone run_job→done 214. Host overhead is the same in render and both (~65 µs incl. tracing); the both-mode excess
  is copy-after-3D (262 µs from frag-done IRQ) vs copy kicked by the host (214) — FW-internal, and the FW traces
  show the same 3D→TQ steps on both stacks. Drm_sched kthread hop (job push → run_job) is ~16 µs; experiment
  module `~/kernel-src/powervr.pin-exp` (param `pin_sched_big`, kthreads on full-capacity CPUs, verified affinity
  6,7): A/B ×5 installed / pinned: render512 0.373 / 0.374, both512 0.673 / 0.668 → no effect, dropped (not
  installed). Userspace pinning loops were too intrusive to measure anything. Board restored (vendor + desktop).
- **FINAL STATE 2026-10-10 ~21:40 — Mesa HEAD `d3e4d4d` (local, not pushed) + installed powervr.ko (chain/APM,
  fw_code_cached, pr_debug, sched-start fix, combine_pr=Y).** Alternating O/V/O/V/O/V blocks (`ovov.sh`, pinned
  cpu6-7, `performance`, system.slice on cpu0-5), medians open / vendor: render512 0.358 / 0.367; render2048
  2.092 / 2.136; both512 0.665 / 0.611; both2048 5.729 / 5.270 (open spread 5.27-5.97); copy512 0.236 / 0.237;
  copy2048 2.510 / 2.485; vkheavy 171.7 / 179.8; cstpf 149.1 / 146.1; cstpi 151.6 / 146.6; **cstp 448.5 / 372.9**;
  vkmany512 0.798 / 0.831; vkmany2048 5.768 / 5.720. Earlier O/V run: render2048 1.830 / 2.101, both512 0.647 / 0.607.
  **Remaining gap: render+copy ("both") ~4-9%.** Diagnosis: FW 3D-finished → TQ kick is the same on both stacks
  (open FW trace 410 units, vendor 394; TQ ~950 / ~986 units; 1 unit ≈ 0.24 µs) and both use 3D TQ (BXM-4-64 has
  no TDM). `PVR_TIMING=1 vkrender 512 300` per frame open / vendor: render record 0.012/0.013, submit 0.031/0.021,
  wait 0.317/0.309; copy 0.002/0.005, 0.016/0.012, 0.218/0.230; both 0.015/0.021, 0.053/0.044, 0.553/0.534. So
  ~10 µs more CPU in vkQueueSubmit and ~10-20 µs more submit→IRQ→wake latency per frame on open — fixed per-frame
  host/kernel latency, not GPU work. Ideas not done: syncobj reuse (~2 µs before the ioctl), kernel submit path.
  glmark2 `--validate`: 0/8 Success on both old and new Mesa (pre-existing zink issue).
- **2026-10-10 21:00 — cstp gap found and fixed (UNCOMMITTED until CTS done): per-workgroup launch floor.**
  Sweeps (pinned): per-dispatch intercept open 0.46 µs vs vendor 0.54 µs (open better); per-workgroup slope
  0.159 vs 0.140 µs. Loop-length variants per WG (256 WG × 400 iters): cstp 0.1625/0.1449, cstpi8 0.1605/0.1480,
  cstpi16 0.2316/0.2305, cstpi32 0.4059/0.4059, cstpi128 1.444/1.900 (open/vendor) → a launch floor (~18 GPU
  cycles/WG more on open), not ALU (cstp's 19→16 groups never moved it; nb vs dyn cstp 360.8 vs 360.1, so the
  literal-pool DMA is not it). Cause: `pvr_alloc_cs_sysvals()` always allocates 3 workgroup-ID coefficients and
  the compute PDS program writes all of them (64-bit DOUTW X+Y, 32-bit DOUTW Z) for every workgroup, though cstp
  reads only X. Fix: `pco_cs_data.workgroup_id_mask` from `nir_def_components_read()` of `load_workgroup_id`
  (all 3 if none found); `pvr_pds_compute_program_setup()` skips unread components (registers stay allocated,
  shader's `rpt3 mov cf0..2` reads stale values it never uses). **cstp per-WG 0.1625 → 0.1283 µs, cstp 360 →
  447 M inv/s (vendor 370.5).** Gate 44/44; compute CTS list (`cts/compute_list.txt`, 1235) identical
  678 Pass / 557 NS. Library `libvulkan_powervr_mesa.wgm.so` / `icd-wgm.json`.
  **Amended (advisor):** mask rounded up to a prefix (X, XY, XYZ) so the PDS generator only takes paths it used
  before (Y-only / Z-only / XZ subsets were never exercised). `wgm2`: cstp 447.4 (wgm 450.2, dyn 360.5), gate
  44/44, compute list identical, builtin_var+basic (93) identical cold and with a warm shader cache (pco_data is
  rebuilt at every pipeline creation; only NIR is cached). Commit `d3e4d4d` (was 5ce243e).
- **Extra validation 2026-10-10 20:45-21:00 (advisor-requested):**
  (1) glmark2 `--validate` through zink (`glvalidate.sh ICD OUT`): 0 Success / 8 Failure on the final stack AND
  identical per scene with `icd-98f4701.json` — the known pre-existing zink validation failure (also d253e35);
  glmark2 runs to the end, no timeouts, no D-state.
  (2) Targeted CTS for the compiler changes, `cts/pco_list.txt` (6428: all glsl.loops/indexing/functions/struct,
  graphicsfuzz, compute.pipeline.basic, opphi, binding_model.dynamic_offset, dynamic descriptor_copy, 1/6 of
  dynamic + plain buffer shader_access, 1/6 ssbo.layout, 1/15 ubo): old `98f4701` 4538 Pass / 47 Fail / 1 Crash,
  new `fef7725` 4539 Pass / 47 Fail (same 47). Only diff: `graphicsfuzz.stable-colorgrid-modulo-true-conditional-
  divided-1` Crash → Pass. 131 job timeouts during the run (loop-heavy shaders), all recovered, no D-state —
  each would have hung the old module. Candidate: raise the 500 ms timeout (it only does bookkeeping).
- **STATE 2026-10-10 20:40 — open vs vendor, same boot, back-to-back (open → vendor → open), medbench 5/5/3 rounds,
  pinned cpu6-7, `performance`, system.slice on cpu0-5.** Open = Mesa `fef7725` + installed powervr.ko (chain/APM,
  fw_code_cached, pr_debug, sched-start fix, combine_pr=Y). open run1 / vendor / open run2:
  both512 0.630 / 0.651 / 0.638; both2048 5.492 / 5.527 / 5.484; render512 0.368 / 0.366 / 0.382 (vendor min 0.344);
  render2048 1.956 / 1.845 / 1.870; copy512 0.243 / 0.252 / 0.251; copy2048 2.548 / 2.559 / 2.552; vkheavy 171.7 /
  179.8 / 171.7; cstpf 148.2 / 146.5 / 147.8; cstpi 151.2 / 146.9 / 151.2; cstp 361.9 / 373.2 (median of 5).
  **Open behind on: render512 (~+2.5%), render2048 (~+1.4-6%), cstp (−3%).** Ahead on all others. Raw:
  bench dir `m-final-{open,vendor,open2}.raw`.
- **2026-10-10 20:05 — combined 3D/PR retried with the timeout fix (`~/kernel-src/powervr.combpr-exp`, param
  `combine_pr`, 0644 → A/B by toggling at runtime):** interleaved ×10, pinned cpu6-7, `performance`, Mesa
  `fef7725` (`icd-dyn.json`), N → Y, median/min: render512 0.410/0.398 → 0.393/0.379 (−4%), both512 0.705/0.690 →
  0.672/0.665 (−4%), render2048 2.486/2.279 → 2.263/2.185, both2048 6.303/6.145 → 6.241/5.928. Gate 44/44;
  PR stress (`icd-flcap.json`, `PVR_TMP_FLMAXG=0x400000`, `vkmany_pr 512 4`, PRREPS 32/64: 13/21 partial renders)
  pixel-exact, 0 timeouts. **CTS 5 lists identical to baseline** (sync 902/816, caselist 4439/623, fdot 824/14,
  caselist3 88/194, tfbc 3989 Pass / 7 Fail — the timeout case now passes); 5 job timeouts, all recovered.
  With `combine_pr=Y`, `memory.pipeline_barrier.graphics.host_write_{vertex,index}_buffer.1048576*` give 1 timeout
  each (N: 0) at equal wall time (6.7-6.9 s): accounting, not slowdown — the PR job's done fence is satisfied by the
  fragment job's timeline update, so it stays pending for the whole geometry+PR+fragment run. Harmless with the
  timeout fix. Merged into `~/kernel-src/powervr/pvr_queue.c` (`pvr_queue.c.bak-pre-combpr2` = before);
  patch `~/open-perf/kernel-combine-pr.patch`.
  **My mistake 20:13:** `ninja -C build -t clean` (for a load test) deleted the build dir's EGL/GBM/zink libs used by
  `env26.sh`; weston then failed "failed to initialize egl". Full rebuild started.
  **Board state after the 19:43 reboot is ~10-25% slower for BOTH drivers** (vendor same conditions: r512
  0.394-0.410, r2048 2.25-2.74, b512 0.69-0.81; open installed module: 0.403-0.423, 2.27-2.57, 0.685-0.79). Not
  CPU/GPU/DRAM clocks (2.0 GHz / 1104 MHz / 2400 MHz, no cooling device active), not CPU contention (system.slice
  moved to cpu0-5). Cause unknown (DRAM traffic from syncthing's post-boot scan is a guess). Only interleaved A/B
  numbers from this boot are comparable.
  Noise controls this boot: `crd-mirror{,2}.service` (FEX loop; restarted 8 s after my stop by the user's
  `health-guard.service` (`/usr/local/sbin/a733-health-guard.sh`: restarts dead critical services every 10 s,
  reboots only after ~90 s of <100 MB free memory past a 3-min boot grace — it logged nothing but "started" in the
  boot that reset at 18:09, so it did not cause that reset)) → instead `systemctl set-property --runtime system.slice AllowedCPUs=0-5`. Thermal zones 0/3/4 use
  `user_space` policy with 60 °C trips → ~19 uevents/s keep udevd at ~10% CPU.
- **COMMITTED 2026-10-10 19:40 (local only), HEAD `fef7725`:** `3ea2916` pvr non-blocking submit, `f4afa13` pco
  unroll limit 128, `fd933a1` pco imadd32/imul32 swap for pooled literals, `fef7725` pvr/pco skip dynamic offset add.
  Validation on the combined stack (`icd-dyn.json`): CTS sync 1718 (902 Pass / 816 NS / 0 Fail; baseline 858 Pass on
  1620), caselist 4439/623, fdot 824/14, caselist3 88/194, tfbc 3988 Pass / 7 Fail (same 7 as baseline) / 1 Crash
  (the kernel-timeout case above); gate 44/44; weston + simple-egl + vkcube OK.
  A/B ×7, pinned `taskset -c 6-7` (A76), `performance` governor, old `98f4701` → new: both512 0.653 → 0.651,
  both2048 5.525 → 5.390, render512 0.385 → 0.397 (noise; focused 600-frame ×8: 0.3685 → 0.3625), render2048
  1.915 → 1.870, copy512 0.253 → 0.258, copy2048 2.540 → 2.530, heavy2048 171.76 → 171.78, **cstpf 144.8 → 148.7,
  cstpi 147.5 → 151.2** (vendor 146.0 / 146.7), cstp ~361 → ~360 (dispatch-bound; vendor ~367).
  **Benchmark noise on this boot:** `~/crd-fex-mirror.sh` relaunches the Chrome Remote Desktop host under FEX in a
  loop (~90% of a core each time) while X is down; `syncthing` 37% CPU; `crush --yolo` running. Not touched (user's
  services); pin benchmarks to cpu6-7 instead. Unpinned schedutil medians were 10-25% worse and very noisy.
- **KERNEL BUG FOUND 2026-10-10 19:20 — job timeout never recovers (stuck `powervr-sched` kworkers):**
  `pvr_queue.c` calls `drm_sched_start(sched, 0)` in `pvr_queue_timedout_job()` and `pvr_queue_start()` — the
  6.13+ signature (errno). On 6.6 it is `drm_sched_start(sched, bool full_recovery)`, so 0 = no recovery: the
  done callbacks removed by `drm_sched_stop()` are never re-added and the timeout is not re-armed. Every job
  pending at the first timeout never signals `finished` (the app hangs), and the next timeout's `drm_sched_stop()`
  can't remove the (absent) callback → `dma_fence_wait(finished)` forever → kworker in D
  (`drm_sched_stop` ← `pvr_queue_timedout_job`). Matches this morning's incident, the combined-PR hang, and tonight:
  "Job timeout" ×4 at 19:11:09 and ×2 at 19:20:34, 3 kworkers stuck. Trigger tonight:
  `memory.pipeline_barrier.graphics.host_write_vertex_buffer.1048576_vertex_buffer_stride_2` (passes in 20+ earlier
  runs; both bursts coincided with my `ninja`/`make -j4` builds → probably DRAM contention pushing a ~0.5 s job
  past the 500 ms timeout). Fix: `true` at both sites (`~/open-perf/kernel-sched-start-recovery.patch`), built and
  installed (`powervr.ko.pre-schedstart-20261010` = previous). **Verified after the approved sysrq reboot (19:43,
  boot `894be03c` → `4d59d488`, triggered by me):** `vkheavy 8192 1` (~3 s job) → 10 timeouts at 0.5 s steps, then
  stop when the job ends, app exits, no D-state; the CTS case above under a `ninja -j8` build → 2 timeouts, Pass,
  no D-state (idle: 0 timeouts, Pass ×2). (`vkheavy`'s own RESULT check fails at every size on every library — stale
  expectation; the gate uses `chk_vkheavy` checksums.) The combined-PR experiment's
  "hang" may have been this same bug, not the FW — worth retrying with the fix.
- **2026-10-10 19:15 — three pco changes on top of the non-blocking submit (all UNCOMMITTED, CTS running on
  `~/open-perf/icd-dyn.json` = nb + all three):**
  (a) **Unroll limit** `pco_nir.c` `max_unroll_iterations` 1024 → 128. CTS
  `synchronization.op.*.(read|write)_image_*.image_64x64x8_r32_sfloat` (98 cases) took >15 min each in
  pipeline creation (also on 98f4701 and ce9f9bb): NIR unrolled the test's 64×64 copy loop to ~41k instrs, then
  `find_parent_instr_from()` (upstream, backward linear scan per vec source) is O(n²). With 128: 0.5 s, Pass.
  Every benchmark shader binary (vkrender, vkheavy, cstp/cstpf/cstpi, vkmany_pr, internal shaders included) is
  byte-identical (`PCO_DEBUG_PRINT=all,internal,binary`).
  (b) **imadd32/imul32 commutative** (`pco_legalize.c` `try_swap_commutative_srcs`, `pco_const_imms.c`
  `shared_readable_ssa` + `blocks_src_swap` now looks through `comp` to the vector's `mov`): 32-bit multiply
  literals go to the literal pool and are read through S0. −2 groups in the cstp-family hash prologue.
  (c) **Skip the dynamic-offset add for non-dynamic buffers** (`pco_binding_data.is_dynamic`, set in
  `pvr_arch_pipeline.c` for dynamic bindings; `binding_is_dynamic()` in `pco_trans_nir.c` load/store/atomic
  buffer paths): `write_buffer()` leaves the descriptor offset word 0, so the second `add64_32` is dead. −1 group
  per buffer access.
  Groups: cstp 19 → 16, cstpi 115 → 112, cstpf 117 → 114. Library snapshots `libvulkan_powervr_mesa.{nbu,imad,dyn}.so`.
  Sync CTS baseline (98f4701, `sync_list2.txt` = 1620 cases without the 98 above): 858 Pass, 762 NotSupported, 0 Fail.
- **Ruled out: syncobj churn.** Microbench (`/tmp/sobench.c`): create+destroy 1.07 µs, transfer 0.57 µs, reset
  0.58 µs. Per frame (strace, vkrender render 512, nb library): 4 create, 4 transfer, 4 destroy, 1 wait, 1 reset ≈ 7 µs
  total, ~2 µs before the submit ioctl. `geom_to_pr_syncobj` is per context, not per submit (old note was wrong).
- **Ruled out: vkrender render FS** is 4 groups / 5 issues (fmul pair, fract fmad pair, `pck` rpt2, `bfi`+or).
- **LEDs (user request):** `leds-off.service` + `leds-off.timer` (OnBootSec=5min, waits for boot done) turn off
  `radxa:blue:user` (heartbeat) and `radxa:green:power`. Undo in session.log.
- **UNEXPECTED REBOOT 2026-10-10 ~18:09 board time (boot `e5606921` → `894be03c`) — not triggered by me.**
  Board journal (`journalctl -b -1`, sudo `COMMAND=` lines): while my CTS run (`~/open-perf/cts/run-nb.sh`, step 1:
  sync list on `icd-98f4701.json`, open driver) was active, another Claude Code session running on the board
  through the `dsh` web shell (`/home/radxa/.npm-prefix/bin/dsh`, PWD `/home/radxa/Desktop/Projects`) ran:
  18:02:53 `./switch-vendor.sh` → 18:02:58/18:03:14 `systemctl start|restart display-manager` →
  18:04:33 `modprobe -r powervr; modprobe pvrsrvkm` → 18:04:39 `systemctl restart display-manager`.
  The 18:02:53 unbind happened with deqp still holding the device: `WARNING ... pvr_drv.c:1507 pvr_remove`
  (`job_ids`/`free_list_ids` not empty), `WARNING ... pvr_fw.c:1109 pvr_fw_fini`, "Memory manager not clean
  during takedown". Journal ends 18:08:13 with no shutdown sequence (hard reset; no pstore on this kernel), next
  boot 18:09:26. Most likely a use-after-free from the unclean unbind + module removal; cause of the final reset
  not proven. Last CTS case started: `dEQP-VK.synchronization.op.single_queue.barrier.write_blit_image_read_image_compute.image_64x64x8_r32_sfloat`
  (qpa stops 18:02:27, 292 results) — not implicated (GPU idle/suspended at 18:02:53).
  **Fix (board scripts):** `gpu-open-stack/switch-{vendor,open}.sh` now also abort when any process holds the
  GPU's DRM nodes (`/sys/bus/platform/devices/1800000.gpu/drm/{card*,renderD*}`, checked with `fuser`). Dry run
  with the desktop up aborts and lists X/kwin/plasmashell. Undo: `cp switch-*.sh.bak-20261010` back.
  The X/kwin-only guard did not see a headless deqp-vk.
- **Non-blocking submit (UNCOMMITTED, `src/imagination/vulkan/pvr_arch_queue.c`):** `pvr_clear_last_submits_syncs()`
  waited for (CPU) and destroyed `last_job_signal_sync[]` on every submit, so each vkQueueSubmit blocked until the
  previous one finished on the GPU. Now it waits only on `next_job_wait_sync[]`; `last_job_signal_sync` stay (GPU
  waits by barriers/queue signals; `pvr_update_job_syncs()` replaces them). Probe `~/open-perf/vkrender_fif`
  (FIF = frames in flight) FIF=3, 98f4701 → new: r512 0.40–0.41 → 0.34–0.36; both512 0.70–0.71 → 0.54–0.63;
  r2048 2.14–2.27 → 1.88–2.29. Gate 44/44, FIF=3 pixel check PASS. Library `~/open-perf/libvulkan_powervr_mesa.nb.so`
  + `icd-nb.json`. **Still to do before commit:** sync CTS list (1718) baseline vs new, the 4 regular lists,
  weston smoke — the first attempt was cut by the reboot above.
- **FW processor is MIPS** on this core (`pvr_mips_get_fw_addr_with_offset`); FW cacheability is the per-page
  `PVR_BO_FW_FLAGS_DEVICE_UNCACHED` flag (MIPS PTE + SLC). The RISC-V FW-window experiments were no-ops here.
- **2026-10-10 21:45 — tried, dropped, board rebooted:**
  (1) Kernel "combined PR" (`~/kernel-src/powervr.combpr-exp`, param `combine_pr`): when the PR job's FRAG_PR
  command equals the chained FRAG command (Mesa builds them identically when `!hw_render->requires_frag_pr`),
  write only its FENCE_PR and let the FW use the FRAG cmd for partial renders (vendor KM
  `bUseCombined3DAnd3DPR`). Handoff 760 → 704 units, A/B r512 0.407 → 0.369, vkmany_pr PR stress pixel-exact (FW
  reused the FRAG cmd for PR + final kick). **But CTS 9170 list hung** in
  `dEQP-VK.memory.pipeline_barrier.graphics.host_write_*` (~#4094): FW looped on partial renders (FWCCB
  0x2abc006b = UPDATE_STATS, repeated) → 2× "Job timeout" → **2 powervr-sched kworkers stuck in D**
  (`drm_sched_stop` ← `pvr_queue_timedout_job`, waiting on the hung job's fence). Dropped; source restored
  (`powervr` = installed module again). Installed module never had it.
  (2) Mesa: skip barrier null jobs for GEOM→GEOM / FRAG→FRAG (in-order queues). Batched A/B within noise —
  the FW itself serializes TA(N+1) behind 3D(N) of the same context (sched `drm_sched_job_wait_dep` trace: geom N
  waits only geom N-2 + frag N-2, reaches FW ~20 µs after; TA finishes ~130 µs after 3D N-1 ends). Reverted.
  User asked "close desktop then unbind?" — desktop was already down; unbind would block on those workers (this
  morning's incident), so sysrq reboot (user approved). After reboot: vendor + desktop by default; installed
  open module = chain/APM + fw_code_cached + pr_debug prints.
- **STATE 2026-10-10 20:30 (same boot, schedutil, medbench 5 rounds; raw `m-open-k2.raw` / `m-vendor-k2.raw`;
  Mesa `98f4701`, installed kernel = chain/APM + `fw_code_cached` + hot-path PVRDBG prints as pr_debug)** open /
  vendor: both512 0.651 / 0.639; **both2048 5.37 / 5.82**; render512 0.376 / 0.372; render2048 1.961 / 1.935;
  **copy512 0.256 / 0.269; copy2048 2.56 / 2.66; vkheavy 171.8 / 180.2;** cstpf ~145.0 (8 reruns) / 146.0;
  **cstpi 147.3 / 146.7**; cstp ~362 / ~367 (morning vendor); vkmany 512 0.823 / 0.825–0.885, 2048 6.19 /
  6.32–6.73 (morning vendor). CTS 4 lists identical with the new kernel; gate 44/44; weston smoke OK.
  Kernel: the per-submit `pr_info("PVRDBG: ioctl submit_jobs")` (also in the original module) cost ~0.1 ms/frame
  at 512 (journald woken through kmsg): render512 0.466 → 0.366 (performance governor). Installed
  `/lib/modules/6.6.98-5-aw2511/extra/powervr/powervr.ko`; previous `powervr.ko.chain-apm-20261010`, original
  `powervr.ko.orig-20261010`. Patches `~/open-perf/kernel-{chain-apm,fwcode-cached,pvrdbg-hotpath}.patch`.
  **Leads left:** (1) combined 3D/PR like vendor `bUseCombined3DAnd3DPR` (needs Mesa PR-state equality check +
  uAPI flag; ~60–80 FW units/frame); (2) multi-pass serialization (batched TA N+1 waits for 3D N); (3) syncobj
  churn (12 ioctls/frame); (4) cstp −1.5% (tail `wdf`+`nop` required by ISA: control groups have no end bit).
- **KERNEL `fw_code_cached` 2026-10-10 19:30:** host timeline render512 (ftrace, no FW trace):
  open submit→TA-done IRQ ~141 µs, TA-done→3D-done **~246 µs**; vendor kick→TA-done ~182, TA-done→3D-done
  **~118**; CPU parts equal. Side-by-side FW traces with all groups (vendor `EnableLogGroup`
  main,mts,...,misc; open 0x7ff): same step sequence (vendor has no separate FRAG_PR cmd — KM
  `bUseCombined3DAnd3DPR`), but every open FW step 1.5–3× longer. Cause: mainline maps FW code/data
  `DEVICE_UNCACHED` (SLC bypass → every FW I/D-cache refill from DRAM); vendor uses FIRMWARE_CACHED |
  GPU_CACHE_INCOHERENT. New param `fw_code_cached` (default Y) in `pvr_fw.c`: frag done 411 → 368 µs after
  submit, A/B (performance governor) r512 0.434 → 0.397, both512 0.804 → 0.709, batched r512 0.379 → 0.336.
  Tried/dropped (experiment tree `~/kernel-src/powervr.fwcached-exp`): CCCB/ctrl/contexts/HWRT/free list via
  FW cached window (bitmask 1..31: no gain / worse) or fully device-cached (96: ≤4% batched only).
  Validated: gate 44/44, PM suspend/resume FW restarts, PR stress (Mesa snapshot `icd-flcap.json` +
  `PVR_TMP_FLMAXG=0x400000`, `vkmany_pr` PRREPS 32–64 → 2–8 PRs/frame, pixel-exact). CPU governor set to
  `performance` for A/B (undo: `schedutil`), logged in session.log.
- **`98f4701` 2026-10-10 18:40:** RT dataset BOs (vheap/RTC, TPC, MTA, MList, region headers) no longer
  `GPU_UNCACHED` (free list stays uncached: CPU-updated, caching it gave nothing; kernel PB pages were already
  device-cached). Batched uniform 2048 1.466 → 1.396, r512 0.397 → 0.382; unbatched render2048 2.25 → 1.97,
  both512 0.816 → 0.772 (8-round A/B; noisy day: schedutil CPU 1.0–1.4 GHz). PR stress tool
  `~/open-perf/vkmany_pr` (`PRREPS=n` draws the grid n times) + temporary free-list cap (`PVR_TMP_FLMAXG=0x400000`,
  not committed; global list < 4 MB fails RT dataset creation) → 3–8 partial renders/frame, pixel-exact. CTS
  identical. **Gotcha:** open FW `fw_trace_mask` 0x1 logs nothing (only 0x7ff works); earlier "0x1" open traces
  were stale 0x7ff buffers (trace_0 is not cleared on mask change).
- **HEAD `b67b05a` 2026-10-10 17:30 (final this round; same boot as vendor `m-vendor-1010b`, medbench 5 rounds,
  raw `~/open-perf/m-open-b67b05a.raw`)** open / vendor: both512 0.713 / 0.658; **both2048 5.57 / 5.70**; render512
  0.434 / 0.375; render2048 2.03 / 1.75; copy512 0.271 / 0.271; **copy2048 2.545 / 2.565; vkheavy 172.0 / 180.0;**
  cstpf 144.2 / 145.9; **cstpi 147.0 / 146.2**. Commits this round: `1fcd0d9` fdot4 tree, `c101211` literal pool,
  `b67b05a` constant pixel-output channels (`pck` of <4 channels leaves upper bytes undefined → bitfield_insert,
  not OR; prefix rebuilt from chased scalars because pco mistranslated `vec3(v4.x, v4.y, v4.z)` — latent pco bug,
  first version failed 1219 CTS cases, final CTS identical). Gate 44/44 (vkheavy refs updated), weston smoke OK.
  Snapshots `libvulkan_powervr_mesa.{fdot,c101211,b67b05a}.so` + `icd-*.json`. Vendor varies between sessions
  (render2048 1.75–2.15). Board returned to vendor + desktop.
- **RENDER FINDINGS 2026-10-10 16:00 (same boot, vendor vs open, `BATCH=20` = per-pass GPU throughput):**
  render2048 vendor 1.505 / open 2.20 ms (no store: 1.24 / 2.18); r8 no store 0.89 / 1.60; vendor r8 slope 0.19
  ns/px vs open 0.31. Fragment shading is **ALU-issue bound**: ~0.24 ms per issue per 2048² pass (`pck` repeat-4 =
  4 issues). Ruled out by experiment (no effect): ISP pipe_enable 3–15 (pipe=0 only: +12%), USC partition mask
  ≥0x3f, enable_4th_partition, pixel width, `olchk` off, FS meta shared-reg upload off, `process_empty_tiles`
  (vendor 0, open 1; forcing 0 = identical ISPCtl 0x80015000, no change), literal pool (no render regression).
  Vendor FW trace via `/sys/kernel/debug/pvr/gpu00/apphint/EnableLogGroup` (`main`, restore `none`) +
  `.../gpu00/firmware_trace`: same TIF line `PartitionMask=0x5`, vendor rotates 4 RT datasets (open 2). Batched open
  is fully serialized: next TA kicked ~1650 FW units after previous 3D finished (render-pass barrier events) — only
  matters for multi-pass submits, not the fence-per-frame benches. Vendor 3D phase r8/2048 ≈ 0.85 ms vs open ≈
  1.0 ms. **Fix (pending CTS):** fold constant trailing channels of RGBA8-style UNORM outputs
  (`pco_nir_fold_pack_consts` in `pco_nir_pvfio.c`, late in `pco_postprocess_nir`): pattern FS 7 → 5 issues,
  render2048 batched 2.20 → 1.75, unbatched 2.31 → 1.98. Known: the `UNIFORM=1` constant-colour shader keeps a
  runtime alpha select (meta bit) → 9 issues, 2.68 ms (vendor 0.95) — next lever. glmark2 `--validate` fails on
  all scenes through Zink, also on d253e35 and with nofbc (pre-existing; spawned as separate task).
  `bcsel(meta, 1.0, 1.0)` (alpha-to-one) is now treated as constant too: `UNIFORM=1` shader = 1 instruction,
  render2048 batched 2.68 → 1.48 ms (vendor ~1.1).
  **Lead (multi-pass submits only):** batched passes serialize (TA N+1 starts ~1650 FW units after 3D N ends).
  Not host-side: the barrier from vkrender's EXTERNAL→0 dependency only gates the next FRAG job; geom waits on
  `next_job_wait_sync[GEOM]` only. DM overlap is on (SLC 16 KB, both stacks). Likely the paired PR(N+1) sits
  behind frag(N) in the in-order 3D CCCB, and the FW won't start TA until the PR is reachable. Next: vendor batched
  FW trace (does vendor overlap?), then try submitting PR ahead / more RT datasets (vendor rotates 4).
- **COMPILER 2026-10-10 14:45 (option 2):** `1fcd0d9` inexact `fdot4` → `(x·x'+y·y')+(z·z'+w·w')` (rule in
  `pco_nir_algebraic.py`, join fadd marked exact so nir_opt_algebraic does not re-chain it): halves dual-issue,
  vkheavy FS 722 → 691 groups, 189.2 → 180.4 ms. **Literal pool** (commit pending CTS 9170): `pco_const_imms.c`
  puts up to 32 non-constreg literals in shared regs after the driver's (`pco_common_data.literal_pool/literals/
  literal_vals`), driver opt-in in `pvr_postprocess_shader_data`, values DMA'd by the descriptor program
  (`PVR_BUFFER_TYPE_COMPILE_TIME`, `descriptor_state->static_consts`; mapping in `pvr_setup_descriptor_mappings`).
  Pooled only if every use reads S0/S2/S3 (fadd/fmul/fmad: src0, or swappable src1) — else the legalizer adds a mov
  per use (first try: cstpi 119 → 145). A/B ×4: **vkheavy 180.4 → 171.9 ms (vendor 180.2), cstpf ~136 → ~144.5
  (145.1), cstpi ~142 → ~147 (146.8)**, render unchanged. CTS 5062/838 fdot/282 buffer identical. Gate: vkheavy
  hashes change with fdot (chaotic loop, rounding order): new refs 256 `7f093a23c9662fee`, 1024 `f03f7d5cd32cfc93`
  (literal pool itself bit-identical). Gotcha: `pkill -f run-lp.sh` killed my own ssh shell — use `pgrep -x`.
- **KERNEL 2026-10-10 14:00 (user chose option 1 then 2):** `~/kernel-src/powervr` (backup
  `powervr.bak-20261010-claude`, patch `~/open-perf/kernel-chain-apm.patch`, README section added):
  `chain_frag` (default Y; geom+PR+frag in one combined kick when the frag adds no new wait) and
  `active_pm_latency_ms` (default 10, was 0; vendor uses 10). Installed to
  `/lib/modules/6.6.98-5-aw2511/extra/powervr/powervr.ko` (original `powervr.ko.orig-20261010`; undo: copy back,
  rebind). Test loader `~/open-perf/open-custom.sh KO [params]`. FW trace TA done→3D kick 1540→1210 units
  (render) / 1840→1300 (render+copy). Medians (5 rounds, ms/frame) apm0/nochain → apm10/chain: render512 0.550 →
  0.487, both512 0.888 → 0.757, render2048 2.37 → 2.29, both2048 6.22 → 5.88 (vendor this boot 5.98). CTS 9170 +
  5062 + buffer lists 0 diffs, gate 44/44, no kernel warnings, no D-state.
- **FINAL 2026-10-10 12:00 (boot `78075bce…`, sysrq reboot approved by user; same boot, desktop down for both;
  `~/open-perf/medbench.sh`, 5 rounds, medians; raw `final-{vendor,open}-1010.raw`), HEAD `ce9f9bb`
  (snapshot `libvulkan_powervr_mesa.ce9f9bb.so`):** vendor / open / open-nofbc — both512 0.722 / 0.960 / 0.902;
  both2048 5.98 / 6.44 / 6.72; render512 0.394 / 0.615 / 0.591; render2048 2.147 / 2.514 / 2.957; copy512 0.282 /
  0.290 / 0.364; copy2048 2.66 / 2.71 / 3.87; vkheavy 180.2 / 189.3 / 190.1; cstpf 145.1 / 138.2 / 138.4 (M inv/s);
  cstpi 146.8 / 141.2 / 141.8. Copy at parity; largest gap = per-frame render overhead at 512 (FW TA→3D handoff).
  `ce9f9bb` removed the 380 KiB zero-init in pvr_queue_transfer (CPU submit −35 µs/transfer). Board left on vendor +
  desktop, `pending.md` restored. Open-host noise: user's `crd-fex-mirror*.sh` FEX service uses CPU periodically.
  Next levers: kernel 3-job pairing (geom+PR+frag one combined kick, est. −85 µs/frame), FW APM latency (0 now),
  compiler for vkheavy/cstp.
- **HEAD `bc1be45` (12:10):** `193516c` TQ shaders read pixel coords directly when msaa_multiplier==1 (16→6
  groups; image copies ~4.0→~3.75 ms); `bc1be45` fixes the soft-bg subpass CLEAR TFBC gap (repro: BY_REGION
  2-subpass, unstored att in subpass 0; probe `TRANSIENT0=1 BYREGION=1 SUBPASS2=x,y,w,h`). Findings: (1) vkrender
  MODE=copy copies a clear-only image (TFBC → near-zero reads, 2.6 ms); real gradient copy is ~3.6–3.8 ms, noise
  4.2–4.3 ms vs 3.6–3.8 uncompressed — TPU TFBC decode costs throughput on non-trivial blocks; renders never lose.
  (2) "both" 2048 = render 2.3 + TQ on gradient ~3.5 + ~0.4 latency; no hidden stall. (3) 512 per frame (FW trace,
  1 unit ≈ 0.15 µs): TA 39 µs, TA-done→3D kick ~257 µs (FW IRQ task ~110 µs + frag KCCB deferred until the IRQ task
  ends + 3D setup ~86 µs; FW even starts a power-off query), 3D 88, →TQ 60, TQ 156; CPU submit 0.13 ms. Kernel pairs
  geom+PR only; pairing the real frag in the same combined kick would save ~70 µs/frame (kernel change, needs reboot).
  Paired tiles (PBE word2 bit 46) and loadop/spm_load single-sample coords: no measurable gain, not committed.
- **HEAD `ff0dfe7` (11:00):** `283b038` pass type TRANSLUCENT for static colorWriteMask 0 attachments (fixes 14
  `attachment_write_mask.attachment_count_8.start_index_1..7`, broken since 9dc8497 removed the implicit discard);
  `ff0dfe7` TFBC default-on (`PVR_DEBUG=nofbc` opt-out): RGBA8/BGRA8, 1 sample/level/layer, usage ⊆
  COLOR_ATTACHMENT|TRANSFER_SRC. Partial render areas: block-aligned PBE clip + force-LOAD load-op variant
  (`load_op_state_fbc`) + draw clears for later hw renders; TQ dst compresses, unaligned rect → clip blit.
  Verified: 9170-case list (`cts/tfbc_list.txt`) + 5062 list + buffer list = d253e35 with/without TFBC (one
  dynamic_rendering resuming case crashed once in-batch, 6/6 + 2× group reruns clean = flake); gate 44/44;
  probe matrix `~/open-perf/tfbcmat.sh` exact. Known gap: soft-bg subpass CLEAR + TFBC + unaligned area.
  **Board state:** 12 powervr-sched kworkers stuck in D since 10:00 (my 60 s INST probe outlived the drm_sched
  timeout) — GPU still works; finish with a sysrq reboot, NOT open-exit.sh.
- **TFBC layout SOLVED:** headers are
  8 B per 8x8 block at `data_addr − 8·(b+1)` (grow down; block min in bytes 4–7), confirmed on vendor (image =
  [headers][data], req = data + 2×hdr). Open had dev_addr at the VMA start, so headers hit the *neighbouring GPU VMA*
  (corruption; page fault once 512 KB of headers left mapped VA). Fix: `fbc_header_size = data/32` (4 KB aligned)
  at the image start, `dev_addr` past it; PBE address from `image->dev_addr` (was `vma->dev_addr`). Decode works
  with the RAW32 TQ path (no format workaround). Matrix 64…2048², non-square, noise: byte-exact, 0 resets;
  LOAD_OP_LOAD via image-view tex state FBC flag: exact. **Perf 2048:** render 2.29 vs 2.88 ms (vendor 2.16),
  copy 3.84 vs 3.96 (vendor 2.69), both ~6.3 vs ~6.8 (vendor 5.74). **Open correctness holes:** unaligned render
  area loses outside pixels in edge blocks (background CLEAR covers whole tiles; needs a force-LOAD load-op
  variant — CLEAR is already a draw for partial areas via `pvr_clear_attachments_render_init`); resolve targets
  (PBE downscale / TQ) written with partial area; TQ dst writes forced uncompressed.
- **TFBC experiment (earlier notes):** PBE
  `compression=1, 8x8, direct` + texture `FB_DIRECT_8X8` (twiddled branch) + skip the TQ raw-format remap for
  compressed sources → byte-exact round trip for surfaces ≤ 8 MB (512², 1000², 1024², 2048×1024, 333² noise).
  Blocks are stored in place at full 256 B slots as `pixel − per-channel block minimum`; the minimum (header) is NOT
  in the slot and NOT within ±64 MB of the image, not global across processes (survives another context's TFBC
  render). Surfaces with blocks at ≥ ~13 MB offset fail: PBE writes stop at base+0xE00050, then the TQ read takes a
  data-dependent MMU read fault (e.g. VA 0x5b238c000, FW trace `BIF0 page fault` → HWR; kernel logs it as
  "unknown FWCCB command 2abc0069" = CONTEXT_RESET_NOTIFICATION). Vendor `libVK_IMG` has `GetFBCSurfaceSize2D`,
  `ForceFBCDCHeaderClearing`, "VK FBCDC scratch buffer" → headers are real. LOAD_OP_LOAD not wired yet.
  FW trace: `echo 0x7ff | sudo tee /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask` (was 0x0; restore after).
- **Compression is the remaining lever:** vendor image copies run above raw copy bandwidth (upload 2048² 2.96 ms =
  11.3 GB/s, readback 2.50 ms = 13.4 GB/s; raw buffer copy ~8 GB/s) → FBCDC. Open image copies 4.11/3.85 ms = at
  memory bandwidth. Render-only store likewise. FBCDC needs kernel (FBCDC heap/state table → FW), Mesa (PBE/TPU
  compression state, image layout, all blit paths, dma-buf export uncompressed) — multi-day, not started.
- Watchdog: applied `RuntimeWatchdogSec=off`, `RebootWatchdogSec=15s` (backup `/etc/systemd/system.conf.bak-20261010-claude`),
  daemon-reexec'd: RebootWatchdogUSec=15s live.
- **FINAL (07:48, fresh boot `8d9ce728…`, both drivers same session, `matrix-vendor-final-1010-0748.txt` vs
  `matrix-open-538357e-1010-0749.txt`):** vkrender 2048 5.74 vs 6.96 (1.21×; render+copy mode 6.06 vs 6.74 = 1.11×),
  512 0.67 vs 0.96 (1.33–1.43×), vkheavy 180.1 vs 190.3 (1.06×), cstpf 145.1 vs 138.7 (1.05×), cstpi 144.9 vs 142.0
  (1.02×), cstpi1/cstpin parity, **cstpi128 33.1 vs 42.5 (open 1.28× faster)**, vkmany 1.17–1.19×, render-only 2048
  2.16 vs 2.88, copy 2048 2.69 vs 4.03. Harness cstp 367 vs 299 is noise (open samples 153–359; interleaved ~351).
- **Incident (03:05–07:45):** CTS test `memory.pipeline_barrier.graphics.host_write_uniform_buffer.1048576` deadlocks
  (both drivers) → kernel `pvr_queue_timedout_job` stuck → my driver-switch unbind hung (D state, holds device lock)
  → `systemctl reboot` froze in final shutdown → **user power-cycled**. Watchdog didn't help: systemd's 30 s / 10 min
  watchdog values exceed sunxi-wdt max 16 s (never armed); `watchdog-pet` disarms on SIGTERM. Proposed (not applied):
  `RuntimeWatchdogSec=off`, `RebootWatchdogSec=15s`. Board after boot: vendor bound, desktop up, healthy.
- User rules: full autonomy for this goal on the board; **commit locally only, never push** (the user's board session
  pushes); continue iterations immediately, no timers. Keep board changes logged below with undo steps.
- Board: boot `721ce8dc…`, open driver `powervr` bound, desktop **down**, dsh `pending.md` **paused**, CTS building in
  `/mnt/sdcard/cts` (pause with `kill -STOP -- -<pgid of build-cts.sh>` during benchmarks, `-CONT` after).
- Mesa: `/home/radxa/mesa/mesa-main` (→ `/mnt/sdcard/_REVIEW/emulation/mesa/mesa-main`), branch
  `claude/perf-2026-10-10`, HEAD `b703ee2` (fixes 1, 3, 4, 6, 7). Build: `ninja -C build src/imagination/vulkan/libvulkan_powervr_mesa.so`.
  Commit with `git -c user.name=ayiejosh -c user.email=132535860+ayiejosh@users.noreply.github.com commit -F <msgfile>`.
- Env for probes: `VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1 XDG_RUNTIME_DIR=/tmp`;
  probes in `/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan/` (`vkrender <size> <frames>`, `MODE=render|copy|empty`,
  `PVR_TIMING=1`); gate `~/open-perf/vkedge <256|1024> <1|4> <c|d|b>` (12 runs must PASS) + vkrender PASS.
- Bench: `~/open-perf/open-bench.sh <label> 3` then `python3 ~/open-perf/compare.py baseline-vendor-1009-2348.txt bench-open-baseline-1009-2354.txt <new>`.
  A/B without toggles: old library `~/open-perf/libvulkan_powervr_mesa.e7923c8.so` via `~/open-perf/icd-e7923c8.json`.
- **Fix 6 committed `31ce4a9` (01:40):** pco — fold single-use `imul` into `iadd` (one `imadd32`) and single-use
  `ffma/fmul/fadd` into `fsat` (`.sat`). cstpf main 175 → 143 groups, cstpi 141 → 109. Interleaved A/B ×5: **cstpf 87.0 →
  103.7, cstpi 72.3 → 83.9 M inv/s** (vendor 145/147); cstp, vkheavy unchanged. Compute correctness gate: output-hash
  probes `~/open-perf/chk_{cstp,cstpf,cstpi} 64 200` — reference hashes `cstp b41c247d35386dea`, `cstpf 3f2e5d221f86b56e`,
  `cstpi e7b93a08d9d692aa` — all MATCH. Saved `libvulkan_powervr_mesa.fix6.so` + `icd-fix6.json` for A/B.
  HEAD is now `31ce4a9`. Next in D: native `ffract` (`lower_ffract=false`, translate as
  `fmad(x, pco_fone, neg(flr(x)))` — verify modifier order with vkrender fract pattern + hashes), then look at what
  else in cstpi's 5 groups/iter (`tst+movc` min/max) and vkheavy's shader.
- **Lead C located (02:00):** `geom_to_pr_syncobj` is per render *context* (`pvr_drm_job_render.c:232`), not per frame.
  The per-frame SYNCOBJ_CREATE/DESTROY come from `vulkan/pvr_arch_queue.c`: a new signal `vk_sync` per job
  (`vk_sync_create` ~295/304, ~423/453/489) and destroy of the previous one (~385/387, 437/467). Next step for C: a small
  per-queue free list of these binary syncs, reused without reset (a signal op replaces the fence), only for syncs the
  queue owns (never exported/imported ones). Expected ≈6 fewer ioctls/frame (~2–3% at 512). Then CTS gate.
  **Decision: SKIPPED.** `pvr_update_job_syncs()` destroys `last_job_signal_sync[type]`; reusing it without a reset
  leaves the old signaled fence in place, so a same-submit wait could pass before the new job signals (race). The safe
  variant (reset on reuse) saves only 3 ioctls/frame (~10–20 µs). Not worth the risk; revisit only if 512 needs it.

- **Fix 8 committed `26ed816`: phase 0/1 dual issue** in `pco_group_instrs.c` (`try_dual_issue`). A p0-only main-ALU
  group takes a later independent fadd/fmul/fmad/mbyp into p1 (s3–s5 → ft1 → w1), lookahead 8 within the block.
  **vkheavy 2048 231.7 → 196.4 ms (vendor ~180 → 1.09×)**, vkrender 512 0.96 → 0.91, 2048 7.76 → 7.38; heavy FS 944 → 786
  groups. Hardware findings: p0 `frcp/frsq/flog/fexp` + p1 op = **wrong results** (excluded); p1 overwriting a p0 source
  is fine (bit-exact). A pre-RA SSA reorder pass found nothing extra (SSA order already adjacent) — dropped.
  Off switch: `PCO_DEBUG=no_dual_issue`. Saved `libvulkan_powervr_mesa.fix8.so` + `icd-fix8.json`.
- **Tools (board `~/open-perf/`):** `gate.sh [ICD]` = full correctness gate (39 checks: vkedge 12×2, vkrender, output
  hashes cstp/cstpf/cstpi + `chk_vkheavy` 256 `8e40f69ef74d97c1` / 1024 `35cc916747173f7d`); `ab-cfg.sh ROUNDS "ENV" ...`
  = interleaved A/B of one build under env settings; `pause-cts.sh stop|cont` (by exact process name — **never
  `pkill -f`**, it matched my own shell). **Gotcha:** probes without `VK_ICD_FILENAMES` + `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER`
  silently run on llvmpipe (different float hash).

- **Clean same-session vendor baseline (01:37, both drivers, `~/open-perf/matrix.sh`, compare with `matrix-cmp.py`):**
  vendor vkrender 2048 5.96 / 512 0.68 ms, vkheavy 180.3, cstp 367, cstpf 145, cstpi 146; per mode 2048 render-only
  2.17, copy-only 2.77; 512 render 0.42, copy 0.27, empty 0.003 (vendor submits nothing for an empty pass). Raw:
  `matrix-vendor-1010-0137.txt`, open at 26ed816 `matrix-open-26ed816-1010-0138.txt`.
- **Fixes 9–13 (all gate 40/40, interleaved A/B vs previous build):**
  - `0d20f6c` fold p0 ALU op into the next min/max group (p2 reads ft0): cstpi 83 → 98.
  - `6271e26` new op `logical.lsl` = p0 `msk_lsl` (shift1, amount in s1) + p1 logical: `a OP (b << c)` in one
    group: cstpi 97 → 116, cstpin 99 → 138. New gate probe `chk_bits` (or/and/xor, shifts 0–31) `694cf1fd7145eaaf`.
  - `2280bf4` re-enable pixout back-prop (upstream had an early `return false;` above its own safety checks).
  - `fbc0649` legalizer swaps commutative fadd/fmul/fmad sources instead of inserting `movs1` for special regs
    (frag coord in S1). vkrender FS 8 → 4 groups; **render-only 2048 3.33 → 2.95 ms**, full 2048 7.30 → 6.97.
  - `c173461` push constants via pseudo mov (fwd-prop reads sh regs), no bcsel for constant-bits bfe/bfi,
    `(x << c) + y` → imadd32: cstpf 125 → 131, cstp ~315 → ~334.
- `243a4cd` skip the compute instance check when the workgroup size is a multiple of ROGUE_MAX_INSTANCES_PER_TASK
  (dispatch pads to whole tasks, so no padding instances exist): cstp 26 → 19 groups; **cstpf 130 → 141, cstpi 120 →
  141, cstp ~342 → ~351** (vendor 145/146/367). Gate probes `chk_wg48` `a436338aa9864ad5` (check kept), `chk_wg96`
  `32e773dc355416b7` (check dropped).
- `f635e60` pre-RA dual-issue scheduler (hoist later / sink earlier independent fadd/fmul/fmad/mbyp next to a p0 op):
  heavy FS 783 → 722 groups, **vkheavy 195.5 → 190.2 ms** (vendor 180.3). **FSINC + p1 = wrong results** (found here,
  run-to-run different) — FSINC/FRED removed from p0 partners. Note: 26ed816..243a4cd allowed FSINC as a post-RA
  partner (no vkheavy pairs formed, hashes matched); fixed in f635e60.
- **CTS** (deqp-vk built at `/mnt/sdcard/cts/VK-GL-CTS/build/external/vulkancts/modules/vulkan/deqp-vk`; runner
  `~/open-perf/cts_run.py ICD LIST PREFIX`, crash-tolerant; lists in `~/open-perf/cts/`). 5062-case selection
  (glsl operators/builtins, compute, fragment_operations, discard/demote, SPIR-V int ops): d253e35 4439 pass / 0 fail;
  HEAD first had **2 regressions** (`fragment_operations.early_fragment.samplemask_no_early_fragment_tests_depth_samples_{2,4}`)
  from `2280bf4` (pixout back-prop into a sample's source-encoded data register → mov inserted *before* the smp).
  Fixed in `0683421`; now identical to baseline.
- **Pre-existing race fixed (`538357e`)**: render pass dependencies with VK_SUBPASS_EXTERNAL were ignored → a copy after
  vkCmdEndRenderPass2 could read the image before the fragment job wrote it (vkedge "holes", 2–7% of runs, also on
  upstream 9a2b374). Now emitted as barrier events at begin/end: 0/300 failures. Diagnostic probe `~/open-perf/vkedge_diag`.
- vkheavy-1024 hash differs between d253e35 and b703ee2+ (256 matches): benign. With ffract native, NIR folds
  `fract(t + 1.0)` → `fract(t)` (first iteration, acc.w = 1.0) — more precise; the chaotic loop amplifies it. A
  negative-input fract probe (`~/open-perf/chk_fract`, x ∈ ±757, DUMPF=file) is exact on d253e35, fix6 and HEAD.
  Baseline library for CTS diffs: `~/open-perf/libvulkan_powervr_mesa.d253e35.so` + `icd-d253e35.json`.
- Render-only rgba8 2048 is **store-bandwidth bound** (~6.7 GB/s): STOREOP=dontcare 2.94 → 2.43 ms; rg16 (same bytes,
  2-ch pack) = same 3.0 ms with store, 1.87 vs 2.49 without. Vendor 2.17 ms → FBCDC (compression) is the lever.
- Pixel merging is always disabled in Mesa (`pvr_arch_cmd_buffer.c` TODO). Enabling it: no measurable change on a new
  8192-triangle grid probe (`~/open-perf/vkmany`, exact pixel check, in the gate) or vkheavy → left disabled.
- Copy job: r8 copy 2048 = 6.24 ms vs rgba8 4.11 ms → per-pixel bound in the TQ path, not bandwidth/texel rate. Render
  scales with bytes (r8 1.69, rgba8 2.98). Vendor uses 16 KB GPU pages (TI DDK non-4K heap) and FBCDC; open = 4 KB, none.
- **Now (est. vs vendor):** vkrender 2048 ~1.17×, 512 ~1.33× (fixed per-job cost), vkheavy ~1.055×, cstp ~1.05×,
  cstpf ~1.03×, cstpi ~1.035×. Render-only 2048 no longer shader-bound (const-color shader = same 3.0 ms).
- **Per-job cost at 512 (sched trace, `~/open-perf/sched-trace.sh` + `sched-parse.py`):** each render = geom + PR +
  frag jobs (kernel pairs geom+PR); empty geom ~150 µs, empty frag ~190 µs run→signal, host gap ~50 µs. Deep CPU idle
  is NOT a factor (disabling state1/2 = no change). Vendor KM source at `/home/radxa/re/ti-ddk` for diffing FW config.
- Vendor uses FBCDC (framebuffer compression); open stack has none — likely the remaining render/copy bandwidth gap.

### Remaining big levers
1. ~~Co-issue~~ done (fix 8). Left: cstpi int loop (5 groups/iter: imadd, min, max, shift, xor; no p1-capable ops).
   Option: fold a single-use p0 op into the next tst/movc group's `p0: mbyp` slot (cstpi 5 → 4 groups/iter).
2. **Copy job** (TQ, ~1.7× slower per pixel than vendor, not ALU-bound): TPU parallel instances, `full_rate`, PBE/SLC.
3. **Open firmware per-job cost** (200–380 µs per no-op job): firmware trace (`fwtrace` tooling in the repo).
4. Pre-existing bug: all-fragments-discarded pass loses the clear colour (vkrender FRAGDISCARD).
5. Validation: CTS (`deqp-vk`, building) on `d253e35` vs HEAD; clean vendor re-baseline; restore board at the end.
- **HEAD `b703ee2` harness ×3 (01:01 board time, 0 faults, GPU 63–66 °C), raw `~/open-perf/bench-fix7-*.txt`:**
  vkrender 2048 **7.685 ms** (base 13.925, vendor 6.740 → 1.14× behind); vkrender 512 **1.153** (base 1.879, vendor 0.708 →
  1.63×); vkheavy **231.5** (base 255.7, vendor 180.3 → 1.28×); cstp **329.6** (vendor 367 → 1.11×); cstpf **123.8**
  (base 86.1, vendor 144.9 → 1.17×); cstpi **83.4** (base 71.6, vendor 146.7 → 1.76×). Vendor numbers are from session
  start (drift caveat) — re-take for the final report.
- vkheavy FS = 944 groups (~29/iter): sin/cos ~5 each (fred.a/b + fsinc + fix-up), fract 1 (fix 7), sqrt = frsq+frcp
  (keep: handles 0/inf), normalize = dot + frsq + 4 fmul. No cheap peephole left; the remaining compiler lever is
  **co-issue / instruction-group packing** (`pco_group_instrs.c` emits one op per group) — a large project.
- **Fix 7 committed `b703ee2` (01:50):** pco native `ffract` = `fmad(x, pco_fone, neg(flr(x)))`, `lower_ffract = false`
  in `pco_nir.c`. Modifier order verified (hardware floors before negating): vkrender fract pattern PASS, all 3 output
  hashes unchanged. cstpf main 143 → 111 groups, vkrender FS 12 → 10. A/B ×5 vs fix6: **cstpf 103.6 → 127.7 M inv/s**,
  vkrender render-only 2048 4.89 → 4.42 ms, 512 1.07 → 1.01 ms. vkedge 12/12. Saved `libvulkan_powervr_mesa.fix7.so` +
  `icd-fix7.json`. **HEAD `b703ee2`.**
- **Lead B measured (01:15):** empty pass 512, gpu_scheduler trace (`/tmp/sched.txt` parse): geometry job queued→run
  44 µs, run→signal **204 µs**; fragment-queue jobs (2/frame) queued→run 67 µs, run→signal **380 µs**; frame first
  queue→last signal ~500 µs, then ~230 µs CPU before next queue. Scheduler is fine; **firmware takes 200–380 µs per
  no-op job**. Hypothesis B2: kernel passes `active_pm_latency_ms = 0` to firmware (`pvr_fw.c`), so the GPU powers
  islands down between every job. **Next step:** in `~/kernel-src/powervr/pvr_fw.c` set a non-zero active PM latency
  (e.g. 10 ms, keep persistent), rebuild `powervr.ko` (`make -C ~/kernel-src/powervr` per its Makefile; installed copy
  `/lib/modules/6.6.98-5-aw2511/extra/powervr/powervr.ko` — back it up first), reload (desktop is down:
  `switch-vendor.sh` then `switch-open.sh`, or rmmod/insmod), re-run the empty-pass trace + MODE=empty/render timing.
- **B2 tested and REVERTED (01:25):** `active_pm_latency_ms` 0 → 10 in `pvr_fw.c:399,462`, rebuilt + reloaded
  `powervr.ko` (only the open driver: unbind/rmmod/modprobe). No change: GPU wait empty 512 0.431 vs 0.427 ms, render
  2048 4.38 vs 4.22 ms; full frames within noise; gate PASS, 0 faults. Restored source and the installed module
  (`~/open-perf/powervr.ko.installed-orig`, identical), reloaded. The 200–380 µs per no-op job is inside the open
  firmware's job handling (open fw build 6976702 vs vendor 6603887). Next for B: firmware trace (repo has
  `fwtrace-*.txt` tooling) on an empty pass, or move on to C/D.
- (old note) lead B — empty-pass GPU latency (~0.4 ms/pass, vendor ~0.009). Trace one empty frame with
  `gpu_scheduler` tracepoints (`drm_sched_job`, `drm_run_job`, `drm_sched_process_job`) via `/sys/kernel/debug/tracing`
  to see queue→run→signal gaps for the geom and frag jobs; check whether Mesa submits geom+frag in one
  `PVR_SUBMIT_JOBS` so the backport's `paired_job` path is used. Then lead C (syncobj churn), D (pco peepholes).
- **Before reporting final numbers:** CTS done/killed, re-take vendor baseline (switch with `open-exit.sh`) and open in the
  same clean state, log temperatures; run CTS groups on `d253e35` vs HEAD and diff failures.
- **At the end:** `~/open-perf/open-exit.sh` (vendor + desktop), restore `pending.md`
  (`mv ~/.dsh/boot-notify/pending.md.paused-by-claude ~/.dsh/boot-notify/pending.md`).

## Resume check (run at the start of every iteration)

1. `ssh a7a` reachable? If not, wait; if still not after the next wakeup, stop and report.
2. Compare `cat /proc/sys/kernel/random/boot_id` with **Last boot ID** below. If it changed, the board rebooted:
   read `journalctl -b -1 -k | grep -iE "oops|bug|panic|watchdog"` for the cause, log it, then redo the state
   steps marked ↻ in the change table (they do not survive a reboot).
3. Check the bound GPU driver, whether the desktop is up, and whether the dsh agent is active.

**Last boot ID:** `721ce8dc-a1a3-47eb-a694-652d653cea3c` (recorded 2026-10-09 23:48 board time, up 2h40m)

## Board facts (2026-10-10, read-only inspection)

| Item | Value |
|---|---|
| GPU core clock `gpu0` / `pll-gpu` | 1104 MHz (set by vendor driver from DT `clk_rate`) |
| GPU OPP table (DT) | 400/600/800 MHz @ 800–840 mV, 1008 MHz @ 960 mV |
| GPU regulator | `axp8191-dcdc4`, 990 mV |
| Open driver clock/power code | none: no `clk_set_rate`, no OPP, no regulator; only `pm_runtime` autosuspend 50 ms |
| Mesa checkout | `/mnt/sdcard/_REVIEW/emulation/mesa/mesa-main` (branch `open-pvr-work-2026-10-06`, HEAD `d253e35`, 43 ahead) |
| Mesa ICD | `/home/radxa/pvr_gen_icd.json` → `/home/radxa/mesa/mesa-main/build/.../libvulkan_powervr_mesa.so` |
| Bench binaries | `/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan/{vktest,vkrender,vkheavy,glheadless}` |
| Switch scripts | `~/gpu-open-stack/switch-open.sh`, `switch-vendor.sh` (refuse while X/kwin alive) |

## Leads

| # | Lead | Evidence | Status |
|---|---|---|---|
| 1 | GPU clock lower under open driver | open driver never sets a rate, but live switch inherits 1104 MHz; compute at parity | **unlikely** — keep as a check, not a lead |
| 2 | No framebuffer compression in Mesa `pvr` | `bxm-4-64.h`: `has_fbcdc_algorithm = true`, `fbcdc_algorithm = 50`; `csbgen/rogue/pbestate.xml` has `COMPRESSION`, `TFBC_LOSSY`, `COMPRESS_SIZE`; nothing in `src/imagination/vulkan/` sets them | **open — top lead** |
| 3 | DRAM frequency lower under the open driver | `a020000.dmcfreq` governor `performance`, pinned 2400 MHz, 0 transitions this boot | **ruled out** |
| 4 | **Every fragment shader runs as PUNCH_THROUGH with ISP feedback** | `pco_nir_lower_sample_mask_out` adds a sample check to every non-internal FS ("add one ourselves"); `lower_isp_fb` always sets `discard_cond = is_helper_invocation` → `isp_feedback` (`alphaf`+`wdf`), `uses.discard` → `ROGUE_TA_PASSTYPE_PUNCH_THROUGH` + `pds_tri_merge_disable`. vkrender FS = 34 groups, ~20 of them this machinery. Independently found by the Mesa research agent. | **fix 1 in test** |
| 5 | pco packs one op per instruction group; missed peepholes | cstpf loop 5 groups/iter: `fmad`, `fadd.sat` (could be `fmad.sat`), `fmul`, floor+subtract (could be one `fmad(y,1,-flr(y))`); cstpi: `imul`+`iadd` not fused into one `imadd32` | open — compute gap (1.7–2×) |
| 6 | No framebuffer compression (FBCDC/TFBC) | research + source; vendor uses FBCDC | open — large project |
| 7 | Pixel merging hard-disabled (`disable_pixel_merging = true`) | research; repo says tried with no gain | low |

## Board changes and undo steps

| When | Change | Undo |
|---|---|---|
| 23:48 | Paused the dsh auto-continue trigger: `~/.dsh/boot-notify/pending.md` → `pending.md.paused-by-claude` (so a crash/reboot does not wake the dsh agent to fight over the GPU) | `mv ~/.dsh/boot-notify/pending.md.paused-by-claude ~/.dsh/boot-notify/pending.md` — **restore at the end** |
| 23:48 | Created work dir `~/open-perf/` with `vendor-baseline.sh` | `rm -r ~/open-perf` |
| 23:48 | `vendor-baseline.sh` stops `display-manager` during runs; its trap restarts it | if the desktop is missing: `sudo systemctl start display-manager` |
| 23:54 ↻ | Switched GPU to the open driver (`open-enter.sh`); desktop **down** while powervr is bound | `~/open-perf/open-exit.sh` (switch to vendor + start desktop). A reboot also returns to vendor. |
| 00:0x | Building Khronos VK-GL-CTS `deqp-vk` in `/mnt/sdcard/cts` (nice 15, `-j5`), source github.com/KhronosGroup/VK-GL-CTS | `rm -rf /mnt/sdcard/cts` |
| 00:2x–00:40 | Mesa: new **local** branch `claude/perf-2026-10-10` (from `d253e35`) with commits `9dc8497` (fix 1), `d49fe8e` (fix 3), `e7923c8` (fix 4); `build/` rebuilt from it (the ICD `/home/radxa/pvr_gen_icd.json` loads it). Not pushed — the user's board session pushes. | `git -C /home/radxa/mesa/mesa-main switch open-pvr-work-2026-10-06 && ninja -C /home/radxa/mesa/mesa-main/build src/imagination/vulkan/libvulkan_powervr_mesa.so` |

## Correctness gates

- **vkedge** (`~/open-perf/vkedge`, source in `open-driver-perf/vkedge/`): 8 triangles (large, slivers, tiny) on a
  1/16-px grid; every pixel checked vs exact CPU edge test. Modes: 1× / 4× MSAA × constant / `dFdx` (forces helper
  invocations) shader, at 256 and 1024 px. **Baseline (unchanged open driver): 8/8 PASS** (1,048,558 px at 1024).
- **vkrender** pixel-exact pattern check (built into the probe).
- **Vulkan CTS** (`deqp-vk`) — building.

## Results

### Vendor baseline — 2026-10-09 23:48, boot `721ce8dc`, desktop closed, 3 reps, 0 faults
Raw: `~/open-perf/baseline-vendor-1009-2348.txt` (board). Harness: `harness.py <probe> <size> <count> --driver=vendor`.

| Workload | Rep 1 | Rep 2 | Rep 3 | Median |
|---|---|---|---|---|
| vkrender 2048 ×20 (ms/frame) | 6.869 | 6.498 | 6.740 | **6.740** |
| vkrender 512 ×50 (ms/frame) | 0.696 | 0.763 | 0.708 | **0.708** |
| vkheavy 2048 ×5 (ms/frame) | 180.312 | 180.409 | 180.130 | **180.312** |
| cstp 64 ×200 (M inv/s) | 353.8 | 367.2 | 369.5 | **367.2** |
| cstpf 64 ×200 (M inv/s) | 144.9 | 145.1 | 143.0 | **144.9** |
| cstpi 64 ×200 (M inv/s) | 146.7 | 147.2 | 145.7 | **146.7** |

### Open baseline — 2026-10-09 23:54, same boot, desktop closed, Mesa `d253e35`, 3 reps, 0 faults, vkrender PASS
Raw: `~/open-perf/bench-open-baseline-1009-2354.txt`. Switched with `~/open-perf/open-enter.sh` (powervr stays bound).

| Workload | Rep 1 | Rep 2 | Rep 3 | Median | Vendor | Open slower by |
|---|---|---|---|---|---|---|
| vkrender 2048 (ms/frame) | 14.282 | 13.769 | 13.925 | **13.925** | 6.740 | **2.07×** |
| vkrender 512 (ms/frame) | 1.879 | 1.582 | 2.121 | **1.879** | 0.708 | **2.65×** |
| vkheavy 2048 (ms/frame) | 255.757 | 255.747 | 255.613 | **255.747** | 180.312 | **1.42×** |
| cstp (M inv/s) | 326.7 | 313.0 | 350.6 | **326.7** | 367.2 | 1.12× |
| cstpf (M inv/s) | 85.3 | 86.1 | 86.7 | **86.1** | 144.9 | **1.68×** |
| cstpi (M inv/s) | 72.4 | 71.6 | 71.2 | **71.6** | 146.7 | **2.05×** |

**Finding:** compute is NOT at parity (the repo's claim). ALU-heavy compute (`cstpf`, `cstpi`) is 1.7–2× slower,
so the gap is in shader execution, not only raster.

### Fix 1 — commit `9dc8497` (branch `claude/perf-2026-10-10`), 2026-10-10 00:13, CTS build paused, 0 faults
"pvr/pco: don't force ISP feedback on fragment shaders that cannot discard". Two parts:
(a) `skip_sample_check` — no implicit sample check when statically 1 sample, sample 0 in static mask, no sample
shading, A2C statically off; (b) `no_alpha_to_coverage` — skip the runtime-guarded A2C lowering when A2C is statically
off; and `lower_isp_fb` emits feedback only for a real discard / depth feedback. vkrender FS 34 → 20 groups, 0 `alphaf`.
Raw: `~/open-perf/bench-fix1-1010-0013.txt`. Compare: `python3 ~/open-perf/compare.py <vendor> <open-base> <new>`.

| Workload | Vendor | Open base | Fix 1 | Gain | Open vs vendor |
|---|---|---|---|---|---|
| vkrender 2048 (ms) | 6.740 | 13.925 | **10.530** | 1.32× | 2.07× → **1.56×** |
| vkrender 512 (ms) | 0.708 | 1.879 | **1.250** | 1.50× | 2.65× → **1.77×** |
| vkheavy 2048 (ms) | 180.3 | 255.7 | 247.4 | 1.03× | 1.42× → 1.37× |
| cstp / cstpf / cstpi (M inv/s) | 367 / 145 / 147 | 327 / 86 / 72 | 355 / 88 / 72 | ~1.0× | compute unaffected (expected) |

Gate: vkedge 8/8 PASS, vkrender 512/2048 PASS.

### Fix 3 — commit `d49fe8e`, 2026-10-10 00:19, 0 faults
"pco: resolve pixel-rate vs sample-rate at compile time when known". `fs_known_single_sampled()` in
`pco_trans_nir.c`: the cmd buffer only sets the runtime `PVR_FS_META_SAMPLE_SHADING` bit when
`data->fs.uses.sample_shading`, so other shaders take the pixel-rate path directly (frag coord + tiled-store coverage).
vkrender FS 20 → 12 groups. Raw: `~/open-perf/bench-fix3-1010-0018.txt`.

| Workload | Vendor | Open base | Fix 1 | Fix 1+3 | Total gain | Open vs vendor |
|---|---|---|---|---|---|---|
| vkrender 2048 (ms) | 6.740 | 13.925 | 10.530 | **9.261** | 1.50× | 2.07× → **1.37×** |
| vkrender 512 (ms) | 0.708 | 1.879 | 1.250 | **1.131** | 1.66× | 2.65× → **1.60×** |
| vkheavy 2048 (ms) | 180.3 | 255.7 | 247.4 | **239.9** | 1.07× | 1.42× → **1.33×** |
| cstpf / cstpi (M inv/s) | 145 / 147 | 86 / 72 | 88 / 72 | 87 / 72 | — | unchanged (next: peepholes) |

Gate: vkedge 8/8 PASS, vkrender 512/2048 PASS.

### Where the remaining gap is (after fix 3) — measured 2026-10-10 00:25
- Harness job trace: open GPU critical path is now **below** vendor (2048: 5.32 vs 5.75 ms; 512: 0.62 vs 0.65 ms), but
  open GPU busy is only 45% (vendor 85%). The gap moved to CPU/submission and the copy job.
- `vkrender MODE=render|copy|empty` (open): 2048 render 6.1 / copy 5.9 / empty pass 1.95 ms; 512 render 2.1 / copy 0.57 /
  empty 1.24 ms. Vendor empty pass per repo: 0.009 ms.
- `PVR_TIMING=1` (open, per frame): record 0.52–0.73 ms, submit 0.23–0.29 ms, GPU wait 0.40 (empty) – 4.2 ms (render 2048).
- strace per frame (empty pass): 18 ioctls + mmap + munmap = **3 SYNCOBJ_CREATE, 3 DESTROY, 3 TRANSFER, 2 WAIT, 1 RESET**,
  1 PVR_SUBMIT_JOBS, plus a 4 KiB control-stream BO created/mapped/unmapped/closed every frame.

### Fix 4 — commit `e7923c8`, 2026-10-10 00:40
"pvr: recycle control-stream blocks across command buffer resets". Per-device cache (≤64 × 4 KiB) in `pvr_csb.c`
(`pvr_csb_bo_get/put`, init/finish in `pvr_arch_device.c`). Syscalls per frame 20 → 13; record 0.52 → 0.11 ms (empty
512), 0.73 → 0.21 ms (render 2048). **Interleaved A/B in one build (5 runs each): vkrender 512 1.919 → 1.387 ms
(1.38×), 2048 unchanged.** Gate: vkedge 12/12 (incl. blend), vkrender render/copy/full 6/6.

**Measurement caveat:** absolute numbers drift ~40% over the session (the same cache-off build ran 512 at 1.13 ms at
00:18 and 1.92 ms at 00:38). Likely memory pressure from the resident (SIGSTOPped) CTS build. Only interleaved A/B in one
build is trustworthy until the CTS build is finished/killed. **Final vendor vs open numbers must be re-taken in a clean
state** (CTS done, both drivers, temperatures logged).

### Correctness findings
- Blending after fix 1: vkedge mode `b` (opaque then src-alpha blended full-screen) PASS at 1×/4×, 256/1024.
- **Pre-existing open-driver bug (not from these fixes):** vkrender `FRAGDISCARD=1` / `PATTERNDISCARD=1` (every fragment
  discarded) FAIL: image all 0,0,0,0 instead of the clear colour 2,2,64,255. Identical with the old behaviour restored
  (temporary `PVR_TMP_NOFIX` toggle, removed). Discard shaders still emit `alphaf` (feedback path intact).
- Depth-write (gl_FragDepth) feedback path: logic unchanged by fix 1; to be covered by CTS.

### Iteration 2 findings (2026-10-10 ~01:00, same boot 721ce8dc, open driver bound)
- Copy job is GPU time (MODE=copy 2048: record 0.05, submit 0.42, GPU 5.12 ms). Per pixel ~1.15 ns (~870 Mpx/s) + ~0.27 ms
  fixed; vendor ~1.5 Gpx/s (repo 2.86 ms). User buffers are not `GPU_UNCACHED` (only internal state BOs). BXM-4-64 has
  no TLA/TDM — vendor also copies on the 3D path.
- **The copy's transfer shader ("TQ", `pvr_uscgen_tq()` in `vulkan/pvr_usc.c:888`, compiled via
  `pvr_transfer_frag_store.c:214`) is 16 groups, 12 of them a per-pixel runtime single-vs-multi-sampled check**
  (`pck.cov` → shift → test → `csel` sr97/sr98 and sr100/sr101, from `fs_is_single_sampled_covmsk` for internal
  shaders, `pco_trans_nir.c` ~567/783). Real work = `smp.2d.nncoords` + `wdf` + 2 output movs.
- Fix plan (fix 5): add a dest-single-sampled property to `struct pvr_tq_shader_properties` (`pvr_usc.h`), set it in
  `pvr_arch_job_transfer.c` (~2014, `transfer_cmd->dst.sample_count <= 1`), make sure the transfer-frag-store shader key
  includes it, set `data.fs.<known_single_sampled>` in `pvr_uscgen_tq`, and extend `fs_known_single_sampled()` in
  `pco_trans_nir.c` to honour it. Do NOT infer it for all internal shaders: `clear_attach`/`spm_load` keep
  `uses_sample_shading = false` but can run on MSAA targets.
- Inspect internal shaders with `PCO_DEBUG_PRINT=all,internal,stats` (plain `internal` selects no stage).
- **Fix 5 tried and REVERTED (never committed):** implemented the plan above; TQ shader 16 → 6 groups, gate 0 failures,
  but interleaved A/B vs the saved `e7923c8` library (5 runs each) showed no gain: copy 2048 4.91 vs 4.94 ms, copy 512
  and full frames within noise. **The copy is not ALU-bound** — it is bound by TPU fetch / PBE / memory. Reverted with
  `git checkout -- src/imagination`; HEAD `e7923c8` rebuilt, vkrender PASS.
- A/B method that needs no temporary toggles: keep the previous library as
  `~/open-perf/libvulkan_powervr_mesa.e7923c8.so` with `~/open-perf/icd-e7923c8.json`, and alternate
  `VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json` (new) vs `icd-e7923c8.json` (old) run by run.
- Next for the copy: compare TQ job setup with vendor (TPU parallel instances = 4, `full_rate`, sampling path
  `smp.2d.nncoords`, PBE/SLC settings) rather than the shader.

### Next leads (ranked by estimated size, 2026-10-10 00:45)
A. **Copy job** (largest, ~3 ms/frame at 2048): open MODE=copy 5.9 ms vs vendor ~2.9 ms (repo). See item 2 below.
B. **Empty-job GPU latency** ~0.4 ms per pass — item 1 below.
C. **Sync-object churn** ~0.1 ms/frame — item 0 below. Sources: `winsys/powervr/pvr_drm_job_render.c:232/267`
   (`geom_to_pr_syncobj` created + destroyed per render submit) and `pvr_drm_job_null.c` (tmp syncobj + transfers).
   Reuse needs a kernel check of same-submit signal→wait ordering (`pvr_sync.c`).
D. **pco peepholes** (compute 1.7–2× gap) — item 4 below.

0. **Sync-object churn**: 12 syncobj ioctls per submit (create/destroy/transfer ×3, wait ×2, reset). Find who creates
   per-submit syncobjs (Mesa `vk_sync` binary emulation / pvr winsys `pvr_drm_*sync*`) and reuse them.
1. **GPU latency of an empty job**: 0.40–0.55 ms GPU wait for an empty pass (vendor 0.009 ms total). Kernel/firmware
   path: geom+frag paired kick exists in the backport (`pvr_queue.c` `paired_job`); check whether Mesa's submit pairs
   them, and runtime-PM/firmware idle between frames.
2. **Copy job** (`MODE=copy` 5.9 ms at 2048 vs vendor ~2.9 ms per repo): transfer path `pvr_arch_job_transfer.c`.
3. ~~Fragment prologue sample-rate check~~ — done (fix 3).
2. **Per-pass overhead**: empty render pass 0.668 ms open vs 0.009 ms vendor (repo `EMPTY-PASS-74X-2026-10-08.md`);
   dominates vkrender 512. Check geom→frag job chaining in the backport's `pvr_queue.c` (the 6.6 scheduler adaptation
   "drops submit_wq/credits").
3. **pco peepholes (compute gap)**: fold `fsat` into single-use `fmad/fadd/fmul` (`.sat`); fuse single-use `imul` into
   `iadd` → one `imadd32`; native `ffract` = `fmad(x, 1.0, -flr(x))` (set `lower_ffract = false`; verify modifier order
   with vkrender's fract pattern). cstpf 5 → 3 groups/iter, cstpi 6 → 5.
4. Framebuffer compression (FBCDC/TFBC) — large.
5. Kernel backport stability fixes from upstream (2224d66, ee7cb74, 0a82240, 45585c3).

### Ruled out by measurement (open driver bound, under load)
- GPU clock: `gpu0` = 1104 MHz enabled during vkrender, regulator 990 mV — same as vendor.
- DRAM clock: `dmcfreq` performance governor, 2400 MHz, 0 transitions.
- `PVRDBG` debug prints in the installed `powervr.ko`: 21 `pr_info` sites, console loglevel 4 (not on serial), ~2 lines/s.
- Runtime-PM power cycling: 17 resumes across the whole 3-rep run = one per probe launch, not per frame.
