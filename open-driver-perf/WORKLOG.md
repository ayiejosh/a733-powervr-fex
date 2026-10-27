# Open PowerVR driver performance — work log

**Goal:** make the open stack (mainline `powervr` backport + open firmware + Mesa `pvr`) match, then beat, the
vendor stack (`pvrsrvkm` + `libVK_IMG`) on the A/B workloads, with no correctness or stability regressions.

**Rules I keep:** nothing is pushed to GitHub without the user's OK; every board change is logged here with its
undo step; no live driver unbind while X/kwin runs (the documented crash trigger).

## 2026-10-12 21:45- (resumed on the user's "continue"; board on open, desktop down, pinned, performance)
- `ZINK_INLINE_UNIFORMS=1` on the loop-uniform scene: no change (619-668 both) — zink does not inline that
  uniform; dropped.
- **Vulkan A/B matrix on HEAD `c737360`** (`~/open-perf/ovov-1012.txt`, 3 alternating rounds, medians, open vs
  vendor): r512 0.362/0.359, **r2048 1.708/1.927 (−11.4%)**, b512 0.644/0.607 (+6.1%), b2048 5.341/5.335,
  c512 0.237/0.244, c2048 2.546/2.546, **heavy 171.2/179.8 (−4.8%)**, cstpf 148.2/146.6, cstpi 151.5/147.9,
  **cstp 445.4/372.7 (+19.5%)**, many512 0.807/0.804, many2048 5.765/5.687. Only b512 is behind (host side).
- effect2d FS (59 groups, 31 temps): 5 `smp.3d.fcnorm.bias.pplod` each with 4-wide coords (u,v,z,0) → ~13
  zero registers + 10 coordinate movs. Cause: pco_nir_tex.c ~538: when `data->common.image_2d_view_of_3d`
  every non-array 2D tex is sampled as 3D with the z slice from `load_tex_meta_pco` (the compiler can't know
  which views are 2D-of-3D); the address-override path then adds a 0 per-pixel lod bias. zink only uses the
  feature for real 3D textures. Experiment: build with the feature off (`n3d`) and A/B texture-heavy scenes.
  Result (`/tmp/n3d-exp.log`): effect2d FS 59→53 groups, 31→26 temps, but FPS only 439-448 → 455-461 (+2-4%);
  texture/jellyfish/bump/shadow/ideas/desktop/terrain ±3%. Not the bottleneck → not adopted (keeps the
  Vulkan feature). The flag is `device->vk.enabled_features.image2DViewOf3D` (pvr_arch_pipeline.c:2964).
- Mac-side outage 22:1x-23:2x: GlobalProtect VPN came up on the Mac (utun4 default route) and broke `.local`
  mDNS resolution; the board (192.168.0.40, uptime 2 days) was fine. Reach it with
  `ssh -o HostName=192.168.0.40 a7a` while the VPN is up. The experiment in flight was lost and re-run under
  nohup (`/tmp/n3d-exp.sh`).
- Experiment `wd` (board `/tmp/pco_schedule.wd.c`, uncommitted, to be reverted): pco_schedule.c rewritten so
  each wdf sits before the first consumer / control op / block end / DRC reuse, requests alternating drc0/drc1
  (only 2 real DRCs; `_PCO_DRC_COUNT` is 3 because it includes PCO_DRC_PENDING — first run used drc2 and was
  invalid). Valid run: effect2d 438/454 → 443/448, texture 1026/1007 → 968/954, jellyfish/bump/shadow/ideas/
  desktop/function ±3% → **no gain, not adopted** (same conclusion as the earlier post-RA sinking attempt).
- 2026-10-12: user routing for agents: haiku = simple collection, sonnet = multi-step/general, opus = problem
  solving / failures, fable = what opus cannot solve (memory `model-routing-haiku-sonnet-opus-fable`).
  Launched read-only workflow `pvr-gap-analysis` (wf_0300ab68-434: 5 topics desktop / effect2d / shadow-terrain-
  refract / alu quality / host fixed cost → analyze → opus verify → fable escalation → ranked plan → critic).

- **2026-10-13 desktop-scene decomposition (open stack HEAD `c737360`, snapshot `vp`).**
  Web research workflow (`wf_2a79b084-270`, 6 topics, haiku sufficed for all, no unsupported citations): see
  digest in the session; key points: render-pass boundaries/load-store are the common tiler lever; the kernel
  only pairs GEOM+FRAG when Mesa submits them in one SUBMIT_JOBS call with the fragment depending on the
  geometry scheduled fence; GitLab/Phoronix pages were blocked (snippet-only).
  Measurements (board, `/tmp/zd-exp.log`, `/tmp/zd2-exp.log`, `/tmp/fbc-exp.log`, `/tmp/vp-exp.log`):
  - ZINK_DEBUG rp/norp/rploads/noreorder: no effect on any scene. PVR_DEBUG=nofbc (TFBC off): slower
    everywhere (desktop shadow 256→241, shadow 255→230, cond0 1198→1030, texture 1100→950) → TFBC helps.
  - desktop blur: 1/2/4/8 windows = 199/146/95/53 FPS (≈1.9 ms per window = 2 passes, flat); blur radius 1/5/9 =
    109/95/87; window-size 0.1/0.2/0.35/0.5/0.8 = 125/117/95/75/50 → ≈7.7 ms fixed with 4 tiny windows.
    desktop shadow: windows 1/2/4/8 = 293/283/256/220 (3.4 ms fixed, +0.16 ms/window).
  - effect2d kernel sweep (open): 1x1 786-879, 3x3 438, 5x5 178, 7x7 105 FPS (≈0.1 ms per extra tap).
  - terrain: bloom/tilt-shift off 21 → 23 → 29 FPS.
  - Per-frame render-pass structure (PVR_RP_DEBUG print, temporary): desktop shadow = 3 full-screen 800x600
    passes/frame (2 CLEAR + 1 LOAD, all STORE); blur windows=1 = 5 passes (2 clear + 2 load full-screen + 1
    window-size). gdb on CmdEndRendering: pass ends are (1) zink_set_framebuffer_state, (2) zink_set_sampler_views
    → resource_memory_barrier (render-to-texture then sample), (3) zink_flush (glFinish) = inherent app
    boundaries; vendor must do the same. GPU timeline: 3 serial fragment jobs of ≈0.4/1.8/1.0 ms per frame.
  - vkrender 1024² MODE=render (ms/frame, open vs vendor): clear b1 0.62/0.64, load 0.62/0.60, dontcare
    0.62/0.63, nostore 0.61/0.56, tiny-area 0.63/0.56, **BATCH=8: clear 0.61/0.49, load 0.60/0.50, tiny 0.58/
    0.41** → pass cost is a fixed floor (load/store/area do not matter); vendor amortizes it with batching
    (0.41 ms/pass) better than open (0.58). Plain-Vulkan passes (0.6 ms) do not explain zink passes of ≈1-1.8 ms.
- 2026-10-12/13: effect2d `wd`/`n3d` experiments rejected (above). Fable review items done.

## 2026-10-13 01:05-01:52 update: clear folding WORKS (commit `281ebd1`, local only, not pushed)

Supersedes the "not adopted / blocker" notes in the section below. Patch:
`backup-2026-10-11/step2/0016-pvr-hold-clear-only-dynamic-render-passes.patch` (commit `281ebd1` on the board,
branch `claude/perf-2026-10-10`, tree clean).
- What changed after cf2: (1) cf3/cf4 replaced the barrier-exposure idea by a **read scan**: descriptor sets
  record the images written into them (`pvr_descriptor_set.images[8]`, overflow = "may read anything"), every
  draw marks pending clears that a bound set may read, and the pass that did so is moved behind a submitted
  clear (`pvr_clear_fold_flush_reads`, relink in `cmd_buffer->sub_cmds`). (2) The real blocker was my own
  layout guard: **zink attaches in `VK_IMAGE_LAYOUT_GENERAL`**, so every consuming pass was rejected as a
  conflict (debug trace: `PUSH → BEGIN → BEGIN → CONFLICT`). GENERAL is now accepted (feedback-loop layouts are
  not folded; sampling the image you render to is undefined in GL anyway).
- A/B (cf5 vs vp, 2 rounds, glmark2 off-screen via zink, pinned cpu6-7): desktop shadow 250/255 → 281/283,
  desktop blur 208/209 → 219/222, shadow 246/242 → 307/310 FPS; refract 45, terrain 22, ideas ~590 unchanged.
- Correctness: CTS `dr_list` (6745) 0 diffs vs `v_dr_vp4.results` (797 pass, 7 known fails), `rp_extra` (1446)
  414 pass / 0 fail, `caselist` (5062), `caselist2` (4213), `caselist3` (282) 0 diffs vs vp; glmark2 --validate
  27 success / 0 failure / 6 unknown on both; no kernel warnings.
- Not covered: the CTS lists are mostly NotSupported for dynamic rendering (5941 of 6745), so the fold paths
  see little CTS traffic; no visual diff of the desktop scenes beyond `--validate`; refract/terrain unchanged.
- Extra checks after the commit (run by Haiku agents on the board, read by me): **CTS sweep** of 12000 sampled
  rendering cases (renderpasses, draw, clear_attachments, image, texture, pipeline blend/depth/stencil/
  multisample; deterministic `shuf` of `all_cases.txt`), vp vs cf5: identical (3703 pass, 10 fail, 8287
  NotSupported, 0 differences). **Controlled A/B, 4 alternating rounds** (vp / cf5 / cf5 noclearfold), means:
  ideas 509/515/516, jellyfish 253/254/253, build 972/959/945, bump 957/952/926, shading 811/804/802, desktop
  shadow 284/313/281 (+10%) → no regression anywhere (the −5% on `ideas` in the first full run was noise).
  **Full glmark2 off-screen score:** cf5 643, vp 641 (the geometric mean barely moves: the gain is on 3 of 33
  scenes). Refract and terrain are not folded (the held clears are flushed by a barrier or non-graphics command
  before pass 3) but both are shader-bound (22-45 ms/frame), so a saved ~1 ms pass is worth only a few percent.
  Switch `PVR_DEBUG=noclearfold` restores the old behaviour. Review before pushing: the relink assumes
  `ended` is the last entry of `sub_cmds` and the submit loop handles each graphics sub-command independently
  (checked in pvr_arch_queue.c).

### 2026-10-13 process review: where this round lost time, and what changed (tools in `tools/`, board copy `~/open-perf/tools/`)
Wall-clock losses, largest first:
1. **2.5 h silent stall (10:46 -> 13:16).** After stopping the serial gate I started the parallel gate with `nohup` but armed no completion
   notification (I had killed the old poller), so nothing woke me. The `cf_list` baseline worker meanwhile hit the runner's fixed 900 s timeout
   10 times in a row on 8-bit subgroup-vote cases (pco compiler loop, same on every build): no stall watchdog, found only when the user asked.
2. **Wrong estimates.** "10-15 minutes" had no measured rate behind it. The first gate ran 15 lists x 2 builds serially on one Cortex-A55 core
   (80 min for 4 lists) and re-ran baselines that were already stored; the same work in parallel on the fast cores took ~15 min.
3. **27 driver switch/restore cycles today** (~1 min each, desktop down each time) and **two false refusals** of the restore guard
   (it counted old kernel log lines) that each cost a manual round (liveness probe + restore by hand).
4. **Self-inflicted rework:** `pgrep -f` killed my own ssh shell (pattern matched the command line); the first variant build never created the
   variant (clone condition evaluated before the lowering sets the flag: one wasted build + gate-less A/B); a lingering `kded6` aborted one switch;
   local Mac commands combining a heredoc and a python check stalled 60 s twice.
5. **Idle while waiting for reviews** (11-18 min per Opus consult) when I did not pipeline other work; long foreground waits (600 s) with no
   interim status.
What changed:
- `tools/cts_run2.py`: stall watchdog (kills deqp-vk when its log stops growing for 120 s, marks the case `Hang`, continues; a hang costs 2 min not 15),
  known-hang list `cts/known_hangs.txt` (8-bit subgroup vote), `--shard I/N`, progress JSON.
- `tools/gate.py`: one command for a differential CTS gate: shards big lists, runs tasks N at a time without pinning, uses stored baselines from
  `cts/baselines.map` (current HEAD results) and runs a base build only when none exists, then **re-runs every differing case alone 3x on both builds
  and reports only persistent differences** (the manual step I did three times), heartbeat in `/tmp/gate.line` (one line: tasks, cases, rate, ETA,
  hangs) and `/tmp/gate.status`.
- `tools/bsession.sh start|stop|status`: ONE open-stack session for a block of experiments; `stop` restores only after a liveness probe (foldtest)
  passes (no log-line counting), never unbinds an unhealthy GPU.
Rules for me from now on: (a) the moment a long job starts, arm a background waiter (`run_in_background` until-loop on a completion string) so it
wakes me; (b) ETA only from a measured rate (read `/tmp/gate.line` at 2 and 5 minutes); (c) every runner has a watchdog and every long job a heartbeat;
(d) one bsession per work block, restore at the end; (e) pipeline: while the GPU gate runs, do CPU work (next patch, build, Opus prompt);
(f) kill by explicit PID or bracketed patterns only; (g) status line when launching anything over 2 minutes, no foreground wait over 5 minutes.

### 2026-10-13 20:30-: broad survey of every layer of the board (workflow `board-leverage-survey`, 32 agents, 0 errors)
User: "check for other leverage that could gain more, do broader on this board". Method: Haiku collectors (read-only: CPU idle/sched/thermal, memory/bus/DT,
GPU power + live vendor config, build flags/tools) -> Sonnet analysts per layer + 4 Sonnet source readers (kernel submit path, Mesa pvr host path, zink GL
layer, pco codegen per pixel) + 4 web-research topics (Haiku, citations re-checked by Sonnet) -> Opus ranker -> Opus refutation of the top 9 -> Sonnet plan.
Flaw found in my own script: the first ranker input was cut at 90 000 characters and never saw the zink/pco/Mesa-host layers; fixed (compact JSON, no
truncation) and re-ranked from the cached agents (`scripts/board-leverage-survey-wf_8364026b-644.js`, run `wf_c849691b-de6`).
- **Verdict:** board-level knobs (cpuidle, IRQ affinity, thermal, THP, DT, NSI QoS, build flags) are about 0..+2% geomean in total; the gap is per-pixel cost
  and per-pass fixed cost. Already-built or retired: FW APM (already 10 = vendor), BVNC table (36.53.104.796 is Imagination's own table), web leads (wrong BVNC,
  tree already 26.3.0-devel), kernel items (KS1/KS3/KS4/KS5: 5-15 us per frame), page size 16K (weakest evidence), GPU clock clamp (device tree, both stacks).
- **Compiler levers (no go needed), candidates PCO1-PCO6, built as experiments in the Mesa tree (uncommitted) behind getenv switches, snapshots pc1/pc2:**
  PCO1 ditr fence modes (`PCO_DITR_MODE` 0 unsafe upper bound, 2 sched=wdf, 3 no fences/wdf kept; mode 3 is the right upper bound: upstream kept the post-fence on purpose),
  PCO4 false pre-fence (`instrs_exclusive`, kill switch `PCO_NO_PREFENCE_FIX`), PCO2 `pco_shrink_vecs` with collated sources (`PCO_NO_SHRINK_FIX`; effect2d 52 -> ~45 groups,
  31 -> ~19 temps expected), PCO3 NaN-preserving fmax/fmin with a constant operand (`PCO_NO_NMAX_FIX`). Opus refutation changed the code before it ran: PCO3 must keep
  `nir_fp_preserve_nan` (else nir_opt_algebraic folds max(min(x,1),0) to fsat: NaN gives 0 instead of 1), only scalar non-NaN constants, constant second in `trans_min_max`;
  PCO4 must skip the exclusive path BEFORE the fence break and require `pif->uniform && !pif->pred_exec`. All three are in pc2. PCO5 (fsat across the versioned join) needs a NIR
  pass `fsat(phi(a,b)) -> phi(fsat a, fsat b)`; the post-RA fold idea is dead. PCO6 occupancy tiers: synthetic probe with N live temps.
- **Pitfall found:** `glbench.sh`/`env26.sh`/`ab.sh` do not set `MESA_SHADER_CACHE_DISABLE`; the shader cache key does not include env switches, so an env-switch A/B inside
  one binary can serve both arms the same cached shader. Every arm now sets `MESA_SHADER_CACHE_DISABLE=1` (in `tools/session2.sh`).
- **New instruments:** NSI PMU (`/sys/devices/platform/soc@3000000/2020000.nsi-controller/nsi-pmu/hwmon0`, per-master DRAM read/write bandwidth in KB/s, calibrated by the display
  engine 288 000 KB/s = 1280x960x4x60; only `pmu_timer` is written, never port_prio/qos/mode) via `tools/nsisample.sh` and `tools/nsitable.py` (bytes per frame, GPU read latency);
  `tools/sysprobe.py` (context switches, interrupts, per-state cpuidle entries, CPU time per thread over a window); `foldtest/nantest.c` + `nantest.spvasm` (NMin/NMax/NClamp matrix with NaN,
  inf, +-0 and constant-NaN operands against a CPU reference; the CTS lists have no real NMin/NMax coverage).
- **Vendor first numbers (800x600, jellyfish 432 FPS, desktop up):** GPU DRAM read 3.05 MB/frame, write 2.53 MB/frame (colour alone is 1.92 MB; colour+depth 3.84 MB): the vendor writes
  less than a full colour+depth store per frame. Vendor apphints seen by the survey: `FBODepthDiscard`, `DisableFBCDC`, `EnableLossyFBCDC`, `EnableStaticPDS`, `EnableMergeRender`.
- **Not covered:** GPU HWPerf counters, vendor shader binaries (idea: `DumpCompilerLogFiles` apphint on the vendor stack), vertex/geometry side of pco, per-pass fixed cost (desktop blur/shadow 36-39%).

- **Session 2 results (build pc2, `MESA_SHADER_CACHE_DISABLE=1` in every arm, 3 interleaved rounds, pinned cpu6-7):**
  all three compiler fixes off -> on: loop +1.9%, function +0.4, ideas +2.1, bump +6.6, shading +1.8, texture -1.1, jellyfish +4.2, **effect2d +15.4, desktop shadow +18.5**,
  shadow +4.4 (about +5% geomean over the ten). Compile dumps: effect2d temps 31 -> 16 (groups 58 -> 52), bump temps 18 -> 16 and groups 233 -> 218, shading 52 -> 48 groups,
  jellyfish 101 -> 98: register pressure (occupancy tier) is the lever, not the group count (PCO6 confirmed by data). NaN matrix `nantest` 130/130 PASS with and without the
  NaN fix, foldtest 20/20 PASS.
  PCO1 is dead: `PCO_DITR_MODE=3` (no fences, wdf kept, unsafe) shading +2.8, jellyfish +3.0, desktop +2.8, shadow +7.1, rest ~0 (about +1.5% at best); mode 0 (no fence, no wdf) jellyfish -31.7%.
  zink env arms dead: `MESA_NO_ERROR=1` and `mesa_glthread=true` within +-3% noise (one +6.8% shadow outlier for glthread).
- **NSI PMU, open stack (KB/s / FPS = MB per frame), same per-frame bytes as vendor except shadow:** loop write 3.0 (vendor 2.8), function 2.85 (2.82), ideas 2.40 (2.33), bump 2.07 (2.15),
  shading rd 1.92 wr 2.68 (1.74 / 2.98), texture wr 2.21 (2.42), jellyfish rd 2.40 wr 3.08 (2.97 / 2.54), effect2d 1.94 (2.05): no depth-discard/compression gap on these.
  **Shadow: open writes 9.5 MB per frame, vendor 3.5** (to investigate: shadow-map pass stores). Desktop row of this first table is unreliable (sample taken during scene set-up).
- **System probe (loop scene):** vendor 12.8 context switches and 9.7 interrupts per frame; open 33 and 20.6 per frame (arch timer 3.1 vs 1.4 per frame). During open runs the user's FEX mirror and syncthing keep cpu4/5 at
  85-98% (not on cpu6-7, but they share DRAM, caches and the thermal budget: 64 C vs 59 C on vendor with the desktop up).

- **Compiler commits (local, NOT pushed), Opus-reviewed, CTS gate PASSED (0 real differences, 14 min), validate 27/0, foldtest 20/20, nantest 130/130 (with a vec store that shares the constants):**
  `37c057c` pco: shrink vecs that have collated sources (also guards `pco_opt_comp_only_vecs`: it counted sources to detect uncollated vecs and a shrunk collated vec could match);
  `c390cb9` pco: skip the NaN expansion of fmin/fmax with a constant operand (constant test via `nir_scalar_resolved` in the lowering AND in `trans_min_max`, because
  `nir_move_vec_src_uses_to_dest` can turn `fmax(0.0,x)` into `fmax(vec.y,x)`); `be232b1` pco: ignore the other branch of a uniform if for DITR pre-fences (skip before the fence break;
  only uniform, non-predicated ifs outside loops). Patches `backup-2026-10-11/step3/`. The experiment switches (PCO_NO_*_FIX, PCO_DITR_MODE) are gone from the tree.
  Full 33-scene glmark2 run with these (single run per scene): geomean **0.712** of vendor (tx1 0.699); effect2d 55 -> 62%, desktop shadow 39 -> 44, desktop blur 36 -> 39, jellyfish 71 -> 75, bump normals 70 -> 73.
- **Register pressure probes (pc3/pc4 switches, timing only):** `PCO_FORCE_FAST` (no slow copy in versioned shaders) A/B: jellyfish +16.7%, effect2d +8.9, ideas +5.1, bump +4.2, shadow +3.3, rest noise.
  Static FS temps/groups default -> force-fast: bump normals 16/218 -> 8/47, bump height 26/108 -> 24/63, jellyfish 20/98 -> 17/36, refract 23/223 -> 19/94, ideas 34/437 -> 30/154.
  **NIR scheduler** (`PCO_SCHED_THRESHOLD=20`, nir_schedule before nir_trivialize_registers): bump +10.1%, shading +9.4%, texture +3.2, ideas +2.8, function +2.0, jellyfish +1.6 but **effect2d -14.1%** (temps 16 -> 27) and
  terrain temps 40 -> 130, refract 23 -> 29, blur 15 -> 27. The gain is not temps or group count (shading phong 48 -> 47 groups, 14 -> 16 temps): the scheduled code puts independent work between a transcendental (frsq/frcp/flog/fexp)
  and its consumer where the default order puts the consumer right behind it. So latency-aware scheduling is a lever; the pressure blow-ups need a threshold or a pick-the-better-compile rule (sweep session6, delay model `PCO_SCHED_DELAYS` in pc5).
- **33-scene shader audit workflow (`pco-audit.js`, 18 agents):** ranked M1..M19: M1 RA live-range holes across the uniform if arms (jellyfish 20 -> 16-17 temps, refract 23 -> 19), M2 null-texture test 6 -> 3 groups (OR the raw dwords in
  `trans_is_null_desc`; `set_p0_from_cond` look-through of mov/xnor), M3 jellyfish constant-alpha blend fold, M4 smp channel trimming (enabler), M5 negated-constant lookup for fmad (cel 17 -> 16), M7 copy coalescing (shadow FS 22 -> 18), M8 fast-only VS/FS
  variants (VS temps: shading 14 -> 9, refract 10 -> 5), M9 uniform loop rotation (loop uniform=true 42 -> 32-37 groups), M10 post-RA copy removal, M13 sqrt(x*x) kept as 3 groups (function medium -10..-15 of ~45), M19 pulsar 16 temps (needs the tier probe).
  Occupancy tier probe `foldtest/occ.c` (N live registers, constant work; sessions7).

### 2026-10-13 17:35-: texture-descriptor checks at draw time (build tx1, working tree, NOT committed)
- **Change** (`backup-2026-10-11/step2/experimental-texchecks-v1.patch`, 6 files +25/-10, board `~/open-perf/experimental-texchecks-v1.patch`, snapshot
  `~/open-perf/libvulkan_powervr_mesa.tx1.so` + `icd-tx1.json`): `record_desc_checks` no longer refuses shaders with texture descriptors:
  it refuses only if `num_descs + num_ubos > PCO_MAX_DESC_CHECKS`, or `num_descs` with the debug switch, or exactly one texture term and no UBO
  (Opus design: with one term the checked copy folds to zeros in-shader, so a draw-time test saves nothing; with two or more both copies stay and
  the draw-time test keeps the temps flat). `pvr_desc_checks_fail` tests only dwords 2-3 of a combined image sampler (the address the in-shader test
  reads). Kill switch `PVR_DEBUG=notexchecks` (bit 7). New `pco_common_data.texture_desc_checks`.
- **A/B (3 interleaved rounds, pinned cpu6-7, `notexchecks` -> default):** bump 916 -> 1018 (+11.1%), shadow 311 -> 330 (+6.1%), jellyfish 283 -> 294
  (+3.9%), loop +3.4%, ideas +2.1%; function -0.8%, shading -0.1%, texture -2.7%, effect2d -1.4%, desktop -1.2% (all inside run-to-run noise).
  Smaller than the `PVR_FORCE_FAST` probe (+30% jellyfish) because the probe also dropped UBO/SSBO checks no draw-time test can prove.
- **Proof the path is live:** `PCO_DEBUG_PRINT=fs,stats` on foldtest scenario 13: default build reads the draw-time register (`mov %4, sh64; bcmp`),
  `notexchecks` build tests `sh6/sh7`, `sh26/sh27` in-shader.
- **foldtest scenarios 13, 14 (new, 15 -> 17 checks):** one pipeline, `sampler2D a; sampler2D b[2]`, one pass, three draws that differ only by
  `CmdBindDescriptorSets` (a null | none null | b[1] null) expecting cyan | white | yellow; 14 adds a CWE flip (on, off, on) between the draws.
  `FOLD_ONLY=13|14`; needs `VK_EXT_robustness2` nullDescriptor (added to the device in foldtest.c). Result: 17/17 PASS on tx1, alt3, tx1+notexchecks.
- **Opus review (24 calls, 177k tokens, read-only): no HIGH finding.** Dwords 2-3 of a combined image sampler are exactly what the in-shader test reads and
  what a null view zeroes (old test over dwords 0-3 gave the same answer; sampler words are outside); descriptor branch of `pvr_desc_checks_fail`
  is correct for arrays/bindings/unbound sets (previously dead code); `desc_checks_reg` is written for VS, FS, alt FS and compute (`pvr_setup_descriptor_mappings`
  called for all three), verdict recomputed on every bind/pipeline change/FS variant flip, the alt FS has its own check list and register. Residual
  LOW items: constant out-of-range `elem` reads past the binding (invalid usage), `range.stride == 0` always takes the slow path, many-texture
  shaders get one more shared register and no compile-time shared-register cap was found (unverified). Pre-existing, not this patch: `write_sampled_image`
  dereferences a NULL `imageView` (a null SAMPLED_IMAGE descriptor crashes at update time although `nullDescriptor` is on; both builds equal in the gate).
  Its one gap: foldtest only covered the fragment shader, so scenarios 15-17 were added: VS samples a, b[2]; FS with one texture + one UBO (null texture |
  both valid | null UBO); compute with a, b[2] writing a storage buffer. Compile dumps show the draw-time register read (`mov %N, shK; bcmp`) in VS, FS and CS
  with the default build and the in-shader `tstz` tests with `notexchecks`. **foldtest 20/20 PASS on tx1, alt3 and tx1+notexchecks.**
- **Gate (15 lists, 4 workers, 20.2 min incl. one 15 min tail from `nullimg`): PASSED, 0 real differences.** Differing cases (all pass 3/3 alone on tx1):
  ubo `random.8bit.nested_structs.2`, pco `graphicsfuzz.spv-composites`, compute `zero_initialize_workgroup_memory.max_workgroups.x/.y` (flaky under
  load, as before), the four 8-bit vote cases (stored Crash, new KnownHang), and nullimg `robustness2...sampled_image...null_descriptor.samples_1.cube_array.frag`
  (stored Crash, tx1 Pass in the batch and alone; not re-run on alt3 yet, see below). Everything else identical.
- **Committed locally (NOT pushed): `9ee9b61` "pvr: test texture descriptors for null when recording draws"** on top of c87374c; patch file
  `backup-2026-10-11/step2/0018-pvr-test-texture-descriptors-for-null-when-recording-draws.patch`, board `~/open-perf/0018-*.patch`.

- **Full glmark2-es2 off-screen (33 scenes, zink, pinned cpu6-7, 10 s per scene; `backup-2026-10-11/glm/full-tx1.txt`) vs vendor native GLES
  (`glm-vendor-off-0123.txt`): geomean 0.699** (the 696bbd6 run, `full-cf5.txt`, scores 0.661 with the same script `tools/glmcmp.py`).
  Best: texture mipmap 95%, buffer 88-93%, conditionals (steps 0) 90%, texture nearest/linear 87/85%, build 79-85%. Worst: desktop blur 36% (110 -> 115 FPS vs
  321), desktop shadow 39% (349 vs 905), effect2d 55/64%, loop uniform=true 59%, shading cel 60%, terrain 60%, ideas 62%, shadow 61%, buffer 63%.
  `glmark2 --validate`: 27 success, 0 failure, 6 unknown (no check for those scenes); kernel errors 0, D-state 0.
- **Why jellyfish is still 71% (compile dumps `PCO_DEBUG_PRINT=fs,stats`, tx1 vs the forced-fast probe `ff3`):** ff3 compiles the two jellyfish fragment shaders to
  20 groups / 7 temps and 36 groups / 17 temps; tx1 (both copies in one program) has 101/95 groups with 11 temps and 92/86 groups with 20 temps. The
  executed fast path of tx1 is ~41 groups (vs 36), so the remaining gap is mostly register pressure (one allocation for both copies) and extra branches,
  not the executed instruction count. Ceiling from the earlier probe: jellyfish +30%, desktop shadow +7%, effect2d +5%, the rest ~0 -> about +1.3% geomean for a
  two-binary design (4 variants with the CWE axis): judged not worth it for now.
- **Fences around varying iteration (new finding, not acted on):** `pco_legalize.c:try_legalize_ditr_fence` puts a fence (flush p0 + two `br.next`: 3 groups) after
  EVERY `ditr/ditrp` ("to enforce WDF ordering") and a pre-fence when an earlier instruction in the linear walk uses an overlapping register. In the jellyfish fast
  path that is 3 fences = 9 of 28 groups. The pre-fence in the else branch is a false positive: the linear reverse walk crosses into the other copy of the
  versioned body (never executed together). Provably safe fix: stop the walk at a branch boundary (saves 3 groups in every versioned fast path). Post-fences:
  `ditr; fence; wdf; ditr; fence; wdf` could batch to `ditr; ditr(drc1); fence; wdf; wdf` but the hardware rule behind the fence is not documented here (upstream history on the board
  is squashed), so that needs a probe + CTS, not a guess.
- **Correction of the 07:00 note (A5):** the installed kernel module already runs `active_pm_latency_ms=10` (vendor's value; param is in `/sys/module/powervr/parameters`),
  `chain_frag=Y`, `combine_pr=Y`. The "APM latency 0, needs go" text came from reading the unpatched source tree. The firmware APM lever is spent; do not ask for it again.
- **Process review of this block (17:35-17:50), waits found and fixed:**
  1. *Gate tail.* `gate.py` ordered tasks by case count, so `nullimg` (1188 cases but ~0.8 s per case: shader compiles) started last and would run
     alone for ~15 min while 3 workers idled (CPU was 57% idle, load 5 of 8: the GPU, not the CPU, is the shared resource). Now `list_cost.json` holds
     measured seconds per case per list (seeded nullimg 0.76, robust_list 0.18, default 0.035; updated after every gate), tasks are ordered by expected
     seconds, shards are cut to ~150 s (`--shard-secs`), and the ETA comes from those costs (the old ETA divided cases by rate and said "1 min" for 5+ min
     twice). Backup `tools/gate.py.bak-pre-cost` on the board.
  2. *Recheck tail.* The "re-run every differing case alone 3x on both builds" step ran without the known-hang list: two compute cases
     (`subgroups.vote.compute.subgroupallequal_{int8_t,uint8_t}` and the `_requiredsubgroupsize` twins) are the same 8-bit pco compile hang, but their names end
     in `int8_t`, which my pattern (`_(u|i)8`) missed. They stalled the gate twice (158 s + 280 s) and would have stalled each of the 6 recheck runs
     (about 30 min). Fix: `cts_run2.py` now defaults `--hangs` to `cts/known_hangs.txt` (so every caller gets it), pattern widened to exactly those four.
  3. *Test code from memory.* The first `foldtex.py` used identifiers that do not exist (`W_`, `run2` shape); reading the real `foldtest.c` first cost one
     call and saved a failed build/run round. Rule: read the target file before generating a patch script for it.
  4. *Hook round trips.* GateGuard denies the first `Write` of every new file (3 extra calls this block). New files are now written in one batch, facts
     stated in the same message; Bash heredocs for scratch scripts.
  5. *Stale heartbeat in big batches.* `cts_run2.py` only counted a case as done when its batch ended, so `nullimg` (one batch of 1188 cases) showed
     0 done for 6+ min. It now counts `#beginTestCaseResult` lines in the qpa log every 10 s (checked: 8 -> 10 of 26 cases in 10 s steps).
  6. *Pipelining that worked:* while the A/B ran (6 min) the foldtest scenario was written; while the gate runs the Opus review, WORKLOG, scheduler fix
     and the compare script (`tools/glmcmp.py`: per-scene ratio and geomean of two glmark2 output files) were done. Remaining serial waits: the GPU gate
     itself and the full glmark2 run (cannot overlap: no GPU benchmark during CTS).

### 2026-10-13 10:10-15:25: alternate FS variant finished: commit `<amended 10524d8>` (local, NOT pushed), gates, Opus review
- **Gate history and why it was slow:** the first serial gate (15 CTS lists x cf6/alt2) ran on one slow Cortex-A55 core (board = 6x A55 @1.8 GHz
  + 2x A76 @2.0 GHz; the runner is one process, the scheduler put it on cpu3) and re-ran baselines that were already stored. cwe2/robust/ubo/nullimg
  finished (0 diffs, robust 12 min per build, nullimg >15 min per build) then I stopped it and ran four parallel workers pinned to cpu6/7/1/2 against
  stored baselines: pco_list 2 min, tfbc2 4 min for both builds, the rest minutes. The CTS gate does not need specific cores (only the FPS A/Bs pin
  cpu6-7); pinning just spread the workers. NOTE: my stop command used `pgrep -f` and killed its own ssh shell (pattern matched the command line):
  use bracket patterns / explicit PIDs.
- **cf_list hang (existing, not from this change):** the baseline cf6 worker hit the 900 s timeout 10 times in a row on the 8-bit subgroup vote cases
  (`dEQP-VK.subgroups.vote.*_u8*/_i8*`, 74 cases): the pco compiler loops at 99.9% CPU; a 90 s check shows the same hang on cf6 and alt2 (exit 124).
  cf_list was run without them (`cts/cf_fast.txt`, 2179 cases): identical on both builds.
- **Results:** foldtest 15/15 (alt2 default, noaltfs, cf6, alt3); glmark2 --validate 27/0; CTS identical for cwe2, robust (2 crashes both builds), ubo,
  nullimg (1 crash both builds; the crash case causes one burst of ~80 `Job timeout` kernel lines, GPU recovers), pco, cf_fast, cf2, tfbc2, loops,
  compute, caselist 1-3, dr_list, rp_extra and the 12000 sample. Single-case differences that appeared while four processes shared the GPU
  (pco_list graphicsfuzz.spv-composites, compute zero_initialize max_workgroups.z, ubo.random.8bit.nested_structs.2) pass 3/3 when run alone on vp,
  cf6, alt2 and alt3 (`cts/recheck*.txt`). The max_workgroups.z recheck produced 713 `Job timeout` lines on all builds (long compute test); GPU alive.
- **Opus review of 10524d8 (27 calls, 195k tokens): ship after fix 1.** (1) the select's dirty-bit write was overwritten later in
  `pvr_validate_draw_state` (worked only by coincidence): the select now returns `flipped` and the caller ORs it in after the overwrite;
  (2) leak of the dropped variant's descriptor bindings; (3) leak of the variant on the compile error path (one helper
  `pvr_destroy_alt_fragment_variant` used by destroy and the error label); (4) rasterizer-discard dynamic state now also marks the fragment
  descriptors dirty. All four applied (build alt3), re-gated: foldtest 15/15, validate 27/0, CTS subset identical to alt2, A/B unchanged
  (noaltfs -> default: loop 764 -> 877, desktop 277 -> 327, effect2d 445 -> 505, jellyfish 248 -> 270, shadow 283 -> 314).
- **Guard note:** the gate scripts compare the kernel's GPU error-line count before and after; old lines in the log (or a rotated ring buffer) make the
  guard refuse the restore even without new errors. In that case: confirm the GPU is alive with foldtest and restore by hand (done twice).
- **Next:** the descriptor-check axis (fast variant without versioned null-descriptor/UBO checks, chosen by a draw-time CPU check): `PVR_FORCE_FAST`
  probe showed jellyfish +30%, bump +9%, ideas +4%, texture +7%; needs texture-descriptor checks at draw time (Opus: step 3 of its plan).

### 2026-10-13 09:00-10:10: alternate fragment shader variant (no CWE select) implemented, gate running (NOT committed)
Opus design review (25 calls, 170k tokens): the plan is sound and smaller than it looks; run a precise probe first because
`PVR_NO_CHECKS` also removed SSBO/dynamic-offset UBO robustness that no draw-time test can prove (tighter probe `PVR_FORCE_FAST`
= force `slow=false` in `pco_nir_version_descriptor_checks`: jellyfish +30% with only provable checks removed, effect2d only +3%).
Order chosen: step 2 first (CWE is the only difference, no descriptor reads), then the descriptor-check axis.
- **Implementation** (working-tree patch `~/open-perf/experimental-alt-fs-v2.patch`, 506 lines, 8 files): `pvr_graphics_pipeline` gets
  `fs_data_alt`, `fragment_alt`, `has_alt_fs`; the pipeline compile clones the FS NIR before lowering when CWE is possible
  (`pvr_cb_color_write_enable_possible(state)`), lowers it with the new `pco_fs_data.no_cwe` flag (the preprocess no longer sets
  `meta_present.color_write_enable`), builds it with the existing helpers by swapping primary/alternate slots
  (`pvr_build_alt_fragment_variant`), drops it if it spills, has no CWE mask, or is not interface-compatible
  (`pvr_fs_variants_compatible`: coeffs, uses, multiview, tile buffers). At draw time `pvr_gfx_select_fs_variant()` (first line of
  `pvr_validate_draw_state`) picks the alternate when `!pvr_fs_cwe_reads_fb(...)` and sets `dirty.fragment_descriptors` on a flip;
  all fragment-shader reads go through `pvr_gfx_fs_data()/pvr_gfx_fs_state()`; shared-register count is the max of both.
  Kill switch `PVR_DEBUG=noaltfs`. First build (alt1) never created the variant: the CWE meta bit is only set during lowering,
  so the clone condition had to be state-based (fixed in alt2).
- **A/B (3 interleaved rounds, noaltfs -> new default):** loop 743 -> 876 (+18%), desktop-shadow 287 -> 332 (+16%), effect2d 457 -> 510
  (+12%), jellyfish 251 -> 278 (+11%), function 555 -> 598 (+8%), shadow 295 -> 317 (+7%), shading +4%, ideas +3%, bump/texture flat.
- **Checks so far:** foldtest 15/15 on alt2 default, alt2 noaltfs and cf6 (4 new CWE scenarios: partial mask, flip back, flip first,
  all off, all inside one pass on one pipeline); glmark2 --validate 27/0; CTS cwe2_list, robust_list (2 crashes on both builds),
  ubo_list: 0 differences. Gate `/tmp/gate-alt2.sh` (15 lists x cf6/alt2, plus the 12000 sweep and a validate) still running at 10:07;
  slower than estimated: robust_list ~12 min per build, nullimg >15 min per build (CPU-bound shader compiles, not hung).
  If clean: commit locally (not pushed), then the descriptor-check axis (needs draw-time texture/UBO checks, see Opus answer).

### 2026-10-13 08:35-08:50: THE per-pixel lever found: in-shader robustness/null-descriptor checks + CWE select (timing probe)
Shader dumps (`scratchpad/dumps/`, `PCO_DEBUG_PRINT=fs,vs,stats`): every FS carries a versioning prelude, texture null-guards
(7-group prologue + per-sample guards with zero-fill else branches), a duplicated fallback body, and at the end a read of po0
under `.olchk` plus a colour-write-enable select. FS group counts: jellyfish 93 (b = 5.75 vs vendor 1.88 ms/Mpx), bump 241,
phong 54, effect2d 59, pulsar 20: per-pixel cost does not track group count, the overhead layers do.
**Probe (timing only, snapshots `ffs`/`ffs2`):** `PVR_NO_CHECKS` (robust_buffer_access=false, null_descriptor=false at shader
build) and `PVR_NO_CWE` (do not set `meta_present.color_write_enable`) in `pvr_arch_pipeline.c`. Mean FPS, 3 interleaved
rounds, base / no-CWE / no-checks / both: loop 756/853/784/861, function 560/602/573/591, ideas 516/533/567/583, bump
899/933/972/984, shading 481/499/483/503, jellyfish 258/278/332/383 (+48%), effect2d 462/477/536/569 (+23%), desktop-shadow
281/308/340/374 (+33%), shadow 291/290/329/318, texture ~1112 (unchanged). CWE select matters on ALU-light scenes, the
checks on texture/descriptor-heavy ones. Both are needed for correctness in general, so the real change is rank 4 of the
earlier plan: two FS variants per pipeline (checked / fast), fast chosen per draw when all colour writes are enabled and the
CPU has verified the descriptors (null images = all-zero dwords, UBO ranges). Opus consulted for the architecture (prompt
included the already-tried list this time). Not committed: probe code was reverted, tree clean at `696bbd6`.

### 2026-10-13 07:00-07:45: why the other 30 glmark2 scenes are at 55-65% of vendor (measured, no code change)
User asked to continue with "the remaining 30" scenes. Reference = vendor native GLES (`glm-vendor-off-0123.txt`),
open = `696bbd6` via zink (`/tmp/full-cf5.txt`). **Geomean 0.66.** Per-scene ratio: desktop shadow/blur 31/34%,
effect2d 51/61%, loop 54-65%, terrain 55%, shading cel 58% / phong 65%, ideas 59%, function 60/63%, jellyfish 61%,
shadow 61%, buffer 61%, conditionals 63%, bump 64%, pulsar 65%; build 80-84%, texture 87-97%, buffer-interleave 84-89%.
- **Not the cause (each measured):** GPU clock (`gpu0` = 1.104 GHz on both stacks during load); raw ALU throughput
  (vkheavy 169.5 ms open vs 179.9 vendor); fp16 (vendor native with `fragment-precision=highp,...` loses 0% on
  loop/conditionals, 5-16% on function/shading/pulsar/jellyfish); zink thread hops (`GALLIUM_THREAD=0` and
  `ZINK_DEBUG=flushsync` change nothing); extra render passes (1 pass per frame on every scene, colour + X8_D24
  depth, both CLEAR+STORE); depth store (timing-only hack dropping it gives +3..12%, loop +7, bump +12, texture +10).
- **Two separate problems (fit frame time = fixed + per-pixel x Mpx from FPS at 400x300/800x600/1600x1200, vendor vs
  open, ms and ms/Mpx):** fixed: loop 0.32 vs 0.68, function 0.33 vs 0.84, conditionals 0.36 vs 0.69, ideas 0.67 vs
  1.12, bump 0.40 vs 0.53, shading 1.01 vs 1.32, texture 0.41 vs 0.36. per-pixel: loop 0.94 vs 1.22, function 1.44 vs
  1.80, conditionals 1.05 vs 1.45, bump 0.70 vs 1.24, shading 0.78 vs 1.41, ideas 0.71 vs 1.38, jellyfish 1.88 vs
  5.75, effect2d 1.17 vs 3.03, texture 0.79 vs 0.98. => shader-light scenes: ~2x fixed cost (+0.35-0.5 ms/frame);
  shading/bump/ideas/jellyfish/effect2d: 1.8-3x per-pixel cost.
- **Host timeline (loop, frame 1.26 ms):** one geometry job (~0.16-0.2 ms) and TWO jobs on the fragment queue per
  frame (second one unexplained, maybe a sync job), GPU has work queued ~80% of the frame; gap after the last
  JOB_DONE to the next RUN_JOB 0.2-0.26 ms (wake chain app thread -> zink gdrv0 -> zfq0 -> submit -> pvr-queue).
  `ideas` is host-bound (46% busy, 1.5 ms gaps). (The first busy% table from a long 0.5 s trace was wrong: tracing
  slowed the host path; use short windows.)
- Files: `scratchpad/glm/` (per-scene results), `scratchpad/tl/` (timelines, busy2.py, vendor-sizes.txt). Scripts on
  the board: `/tmp/clkprobe.sh`, `/tmp/busy-run.sh`, `/tmp/tl-run.sh` + `/tmp/hosttl.sh`, `/tmp/sizes-run.sh`,
  `/tmp/thr-run.sh`, `/tmp/pass-run.sh`, `/tmp/nds-run.sh` (all restore the vendor stack at the end, and stop a
  lingering `kded6` by PID first: it held `/dev/dri/renderD128` once and the switch guard aborted safely).
- **Opus consult (28 calls, 195k tokens) and what I verified of each claim:**
  (A1) each render is 3 kernel jobs in one ioctl: GEOM, a PR (partial-render) FRAGMENT job, then the real FRAG
  (`winsys/powervr/pvr_drm_job_render.c` ~470-640) — CONFIRMED in the source; dropping the PR job (timing hack
  `PVR_SKIP_PR`) makes every scene ~6x slower (~100 FPS): the kernel pairing needs it, idea dead.
  (A4) "emulated timeline type chosen" — the switch `PVR_NATIVE_TL` (leave `sync_types[1]` NULL) changes NOTHING
  (same 3 CREATE / 3 TRANSFER / 3 DESTROY per frame, FPS within noise). The churn is the queue's own per-submit
  syncs (`pvr_arch_queue.c`: geom/frag signal syncs created per render, old `last_job_signal_sync` destroyed in
  `pvr_update_job_syncs`): 11 ioctls/frame, ~86 us of sync churn + 51 us SUBMIT_JOBS per frame (~7% of the 1.26 ms loop
  frame). An earlier 5-round attempt (`bench/pvr-vulkan/SYNC-TIMELINE-ATTEMPT-2026-10-08.md`) got a GEOM-only
  migration working but measured no gain on the Xwayland metric and reverted. For off-screen glmark2 the churn is on the
  critical path every frame, so a real but small gain (~5%) is possible; cost: several rounds, fragile.
  (A5) APM latency 0 in the kernel FW config: no per-frame PM messages in the kernel log, runtime-PM autosuspend is 50 ms
  (irrelevant at frame rate); not testable without a kernel/FW change (needs the user's explicit go).
  (B1) "wdf after every smp" — ALREADY TESTED earlier as experiment `wd` (deferred wdf, alternating DRCs): no gain.
  I forgot to put the already-tried list in the consult prompt (rule 1 of the efficiency rules): do it next time.
- **Also measured:** glmark2 `--frame-end` default is `finish` on both stacks (vendor default 1366 = finish 1342; `none`
  30601 is just queueing), so both block per frame. Vendor native GLES loop fixed cost (0.32 ms) is half of vendor's own
  Vulkan pass floor (0.64 ms): the vendor GL driver's path is much leaner than any Vulkan-based one.
- **Cleanup item:** `pvr_drm_job_render.c` has `getenv("PVR_SUBMIT_MIX")` and `getenv("PVR_JOB_TRACE")` debug code in the
  per-submit path (earlier sessions): remove before any upstream PR.
- **Where the remaining gap is (summary):** (1) fixed cost ~+0.35-0.5 ms/frame = ~0.23 ms host chain (ioctls, wake chain,
  zink+pvr recording spread thin over many functions, none above ~3%) + ~0.2 ms extra GPU-side job floor (GEOM 0.16-0.2 ms
  for a quad); (2) per-pixel 1.8-3x on shading/bump/ideas/jellyfish/effect2d (shader/TPU side, not diagnosed). No single
  cheap lever found; each remaining item is a multi-round engineering task (see next steps in the final status).

### 02:00-02:35: delegated checks, Opus review, fixes (commit amended to `696bbd6`, still local)
User routing for the extension to 03:00: haiku runs board commands / research, sonnet reads results and thinks,
opus is consulted for review. Used: 5 Haiku runs (CTS sweep, ideas A/B, fold debug trace, fix gate, sweep2;
each 3-6 tool calls, ~95-105k tokens) and 1 Opus read-only review (21 tool calls, 217k tokens, ~18 min).
- **Opus review of `281ebd1` found real bugs the CTS cannot see** (each needs a pattern CTS does not record):
  (1) HIGH legacy `vkCmdBeginRenderPass2` after a held clear: the nested flush overwrote `render_pass_info`
  (dr_info/hw_render/rstate/attachments) → crash or wrong render; (2) HIGH the `gfx.has_query` hold test never
  fires (it is only set in `end_sub_cmd`) so a clear-only pass with an occlusion query was held and the next
  sub-command inherited `state->query_indices`; (3) HIGH `flush_reads` moved only the last graphics sub-command
  of the pass, not its event/query-availability/ZLS copy-back/resolve sub-commands; (4) MEDIUM a held clear was
  dropped when the next pass loads with `STORE_OP_NONE` (or `LOAD_OP_NONE`); (5) MEDIUM a pass that reads the
  image whose clear it took over (read-only GENERAL attachment that is also sampled) saw old memory;
  (6) LOW no error check after the nested flush; (7) LOW dangling `render_pass_info.rstate`; (8) LOW memory
  aliasing / buffer barriers not considered (comment only).
- **Fixes (all in `696bbd6`)**: flush at the top of `CmdBeginRenderPass2` and for every non-graphics
  `start_sub_cmd`; hold test uses `gfx.query_pool` and `state->query_indices`; `fold_pass_prev` marks the list
  position before the pass and all of the pass's sub-commands are moved behind the clear sub-commands;
  `pvr_attachment_ops_allow_fold()` (load LOAD/CLEAR/DONT_CARE and store STORE/DONT_CARE only); consumed
  clears stay visible to the read scan and are submitted if read; OOM break/return; `rstate = NULL`.
- **Gates on cf6 (the fixed build):** CTS dr_list/rp_extra/caselist/caselist2/caselist3/12000-sample all
  0 differences; glmark2 --validate 27/0/6, kernel 0, D-state 0; A/B vs vp: desktop shadow 258/255 → 286/279,
  desktop blur 209/205 → 224/222, shadow 242/234 → 312/308, ideas 600/555 → 589/570 (flat).
- **sweep2 (9000 sampled query_pool / synchronization(2) / dynamic_rendering / renderpass2 / command_buffers /
  copy_and_blit cases), vp vs cf6: identical** (1600 pass, 6 fail, 15 DeviceLost, 7379 NotSupported on both; the
  15 DeviceLost and the repeated kernel line `Received unknown FWCCB command 2abc0069` come from the stock
  driver as well, GPU idle and healthy afterwards: no hang/reset/fault, no D-state).
- **Cost of this stretch (02:00-02:41):** Opus review 6.8M cache-read / 0.5M new / 73k out (44 turns); five
  Haiku runs 3.7M cache-read / 1.45M new / 22k out in total. Together ≈ 5% of the 217M cache-read tokens of the
  `pvr-gap-analysis` workflow, and they produced the committed change, its gates and the review that found the
  bugs. Rule that held: Sonnet writes the script (quoting), Haiku starts/polls/returns the log, Opus gets one
  specific list of questions with a turn cap.
- **foldtest (written 02:45-02:52, `foldtest/` next to this file; Haiku ran it on the board):** a small Vulkan
  program with 8 scenarios (held clear at end of command buffer, copy in the same command buffer, legacy render
  pass after a held clear, occlusion query in a clear-only pass + query result, STORE_OP_NONE after a held
  clear, consume LOAD/STORE, a draw that samples the held-cleared image, the zink shape). Results: vp, `696bbd6`
  (cf6) and cf6 with `PVR_DEBUG=noclearfold` pass 9/9. The **first commit `281ebd1` (cf5) fails exactly where the
  Opus review said**: scenario 2 segfaults (exit 139), scenario 3 query result never available (NOT_READY),
  scenario 4 leaves the stale green; scenarios 0, 1, 5, 6, 7 pass on cf5 as expected. So the test detects review
  items 1, 2 and 4 and confirms the fixes for them. Scenario 6 (sampling a held clear) passes on both, so the
  read path is exercised at least in its simple form.
  Scenario 8 (added 02:52: occlusion query around the draw of the reading pass, sample count 4096 expected)
  passes on vp, cf6 and cf5: the query-availability write that cf5 moved ahead of the pass is a transient window
  that the final state does not show, so review 3a stays review-only. **Still not covered:** relink with
  ZLS copy-back / resolve sub-commands in the reading pass (review 3b, 3c), split passes (3d), and a pass that
  reads the image whose clear it took over (review 5). All covered by code review only.
- **Previously listed as not tested (now partly covered, see above):** no test created the review's failure patterns on purpose (legacy pass right after a
  held clear, occlusion query in a clear-only pass, STORE_OP_NONE after a held clear, sampled+attached same
  image). They are covered by review and by `sweep2` (query_pool / synchronization / dynamic_rendering sample)
  only. Before pushing: write a small Vulkan test for those four cases (vkrender bench in
  `/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan` is the place).

### State at 02:45 and next steps
- Board: vendor stack + desktop, schedutil, `AllowedCPUs` cleared, crd-mirror active (`session.log` has the
  entries). Mesa tree `~/mesa/mesa-main` clean at **`696bbd6`** (local commit, **not pushed**; patch
  `backup-2026-10-11/step2/0016-pvr-hold-clear-only-dynamic-render-passes.patch`). Snapshots on the board:
  `~/open-perf/libvulkan_powervr_mesa.cf6.so` + `icd-cf6.json` (fixed build), `...vp.so` (HEAD before the fold).
- Before pushing it as a PR: (1) a small Vulkan test for the four patterns CTS does not record (legacy
  `vkCmdBeginRenderPass` right after a held clear; occlusion query in a clear-only pass; `STORE_OP_NONE` after a
  held clear; image both attached read-only and sampled); (2) ask the user for the push OK (rule: nothing is
  pushed without it; patches go to ayiejosh/a733-powervr-fex, see memory `a7a-open-driver-pr-target`).
- Next performance targets, in order: desktop shadow/blur are still ~1/3 of vendor (two serial passes of ~1-1.8 ms
  each: look at the blend/pulsar cost in pco and the per-pass floor, vendor 0.41 vs open 0.58 ms at BATCH=8);
  terrain 22 FPS and refract 45 FPS are shader-bound (uniform hoisting for shading/bump is still open);
  refract/terrain clears are held but flushed by a barrier or non-graphics command before pass 3 (low value).

## 2026-10-13 night, final status (stopped 01:01 board clock, on the user's "stop at ~2am, don't burn the quota")

**Board restored:** vendor `pvrsrvkm` + display-manager active, `schedutil`, `system.slice AllowedCPUs` cleared,
crd-mirror active, `~/.dsh/boot-notify/pending.md` present; logged in `~/open-perf/session.log`. Mesa tree
`~/mesa/mesa-main` clean at `c737360`. Nothing committed or pushed tonight. (`switch-vendor.sh` printed one
harmless `Device or resource busy` on its explicit bind: `modprobe` had already bound the vendor driver.)

**Pending-clear fold (experiment `cf`; patch `backup-2026-10-11/step2/experimental-pending-clear-fold.patch`,
also `~/open-perf/experimental-pending-clear-fold.patch`, 572 lines, builds, NOT adopted).**
Idea: do not submit a clear-only dynamic render pass (loadOp CLEAR + STORE, full view, no draws); hold it as a
pending clear and turn the LOAD of a later pass on the same image into a CLEAR; flush it (submit it at that
stream position) on anything else that could see the image (non-graphics sub-command, other attachment use,
resolve, barrier naming the image, ExecuteCommands, EndCommandBuffer). Switch: `PVR_DEBUG=noclearfold`.
- Mechanism works: 720 of ~1440 passes per 3 s are held and consumed (debug counters).
- **Blocker:** zink emits a *global* `VkMemoryBarrier2` between the passes — src COLOR_ATTACHMENT_OUTPUT /
  COLOR_ATTACHMENT_WRITE, dst FRAGMENT_SHADER / SHADER_READ (`src=400/100 dst=80/20`, 444 per 2 s). A global
  barrier cannot tell us which image a later draw samples, so the only correct reaction is to flush, and with
  that flush the fold saves nothing (A/B vp vs cf vs cf+noclearfold: all within ±3%).
- **Upper bound** (timing-only run that ignores global barriers — can render wrong, never ship): desktop shadow
  250→283/287 (+13%), desktop blur 208→219/224 (+6%), glmark2 shadow 241→302/312 (+27%), refract 45→44 (none:
  its 1600x1200 passes differ), terrain 22→22. So the lever is real (≈ +13..27% on 2 scenes), the cheap
  version is unsafe.
- **cf2 (01:1x, same night): exposure tracking, no gain.** A fragment-only global barrier no longer flushes; it
  marks pending clears `exposed`, and `vkCmdBeginRendering` consumes an exposed clear or submits it before the
  pass (frag→frag is implicit on this HW, so a late flush right before the next pass stays ordered). Built and
  A/B'd (vp / cf2 / cf2+noclearfold, 2 rounds, 8 scenes): all within ±3%, no gain. Debug event trace per desktop-
  shadow cycle: `PUSH` (empty CLEAR of X held) → `BARMARK` → next pass = CLEAR of a *different* image Y (25
  draws) → `EXPFLUSH` → pass 3 = LOAD of X. The barrier is pass 2's own "render-to-texture then sample"
  barrier, so X is flushed before pass 2 and never reaches pass 3. To hold X across pass 2 the driver must
  know pass 2 never samples X: that needs a draw-time scan of the bound descriptors (only while
  `pending_clear_count > 0`) and, in the rare "pass 2 does read X" case, submitting X's clear *before* the
  already-recorded pass-2 sub-command (relink in `cmd_buffer->sub_cmds` at EndRendering). Not attempted tonight:
  too intrusive to verify with CTS before the 02:00 stop. Patch in the file above was refreshed to the cf2 state
  (614 lines, clean of debug code).
- **Decision recorded (1 vs 2):** same measured ceiling (+13% desktop shadow, +27% shadow, +6% blur), so gain
  alone does not separate them. Option 2 (pvr) is the smaller change but needs the descriptor scan + reorder.
  Option 1 (zink) removes the empty pass at the source (best long-term, upstreamable, also helps other tilers)
  but zink's pending clear is tied to the bound-framebuffer slots (`ctx->fb_clears[]`, `clears_enabled`,
  `rp_clears_enabled`) and to ~10 resource-access hooks (`zink_fb_clears_apply*` in zink_blit.c/zink_clear.c),
  which only scan the *current* framebuffer; a detached pending-clear list would have to be added to all of them.
  Next session: start with the zink design only if there is a full day; otherwise do the pvr scan+reorder with
  the CTS lists (`~/open-perf/cts/dr_list.txt` 6745, `rp_extra.txt` 1446; baseline `cts/v_dr_vp4.results`).
- **Ways forward (older notes):** (a) in pvr, flush-by-reorder: when a draw/dispatch binds an image view
  that has a pending clear, create the clear pass and insert it *before* the current sub-command in
  `cmd_buffer->sub_cmds` (descriptor scan only while `pending_clear_count > 0`); intrusive, needs CTS dr_list +
  rp_extra + caselist. (b) in zink: stop flushing the pending clear as an empty pass at
  `zink_set_framebuffer_state`/`zink_flush_clears` (zink_context.c ~3731 and ~4238) and fold it into the next
  pass on that FBO there; upstreamable, no descriptor scan, but touches more zink paths. I would try (b) first.

**Workflow cost analysis (tokens from the agent transcripts; `workflow-cost.py` re-computes it).**
| workflow | agents | new input | cache read | output |
|---|---|---|---|---|
| `pvr-gap-analysis` (wf_0300ab68) | 20 | 11.2M | 216.6M | 1.30M |
| `pvr-web-research` (wf_2a79b084) | 13 | 3.8M | 42.1M | 0.33M |
| design review by Opus (killed, no result) | 1 | 0.4M | 6.6M | 0.03M |

gap-analysis by stage: **analyze (5 × sonnet, 105-142 turns each) = 143M cache read, 0.71M output = 66% of
the workflow**; verify (5 × opus, 45-65 turns) = 43M / 0.19M; escalate (3 × fable) = 14M / 0.11M (+2.1M new
input: they re-read the same context); collect (5 × haiku) = 11M / 0.22M; critique + synthesize = 4.6M / 0.07M.
- **What paid off:** collect (haiku) — cheap, factual, and the later stages rely on it. Verify — the adversarial
  pass refuted ≈33 of ≈61 verdict claims (e.g. "chain_frag already implemented", "ZLS write dead on this core",
  "IRQ thread cannot move to the hard handler"), so the plan was not built on wrong ideas. Haiku was enough for
  all 6 web-research topics; the sonnet verify there (6 agents, 8.6M) found no unsupported citation.
- **What was waste:** (1) analyze ran 100+ turns each because the agents explored the repo from zero — most of
  their proposals were already in this WORKLOG (already built, already measured 0%) and died in verify, i.e.
  we paid twice (analyze + verify) for ideas a 2-page "already tried" list would have removed. (2) Fable
  escalation re-read the whole context to answer 3-4 narrow questions each; 2 of 3 returned "refuted/no new
  mechanism". (3) The Opus design review explored zink and pvr for 47 turns / 6.6M cache-read tokens and returned
  nothing; a 20-minute A/B (the global-barrier finding above) answered the real question. (4) Nothing in the
  plan was decided by a number; every real result tonight (empty clear passes, +10-22% ceiling, barrier
  blocker) came from a 5-line debug print and one A/B, not from an agent.
- **Rules for next time (fast, high knowledge, low tokens):** 1. Put the "already tried / measured" list and
  the relevant 3-5 file paths in every analyze prompt; cap analyzers at ~40 turns and ask for ≤5 ranked ideas.
  2. Verify only the top 5 ideas, one verifier each, and only ideas that need no measurement; measure the rest.
  3. Escalate to fable/opus only with a single yes/no question and the exact evidence, never "review this".
  4. Prefer "instrument → run one A/B → read the number" over an agent whenever the board can answer in
  <10 min (it answered 3 times tonight faster than any agent). 5. Stop an agent after 25 turns without a
  concrete file:line finding. 6. Keep haiku for collection and web search; use sonnet only to merge.
- **Rules applied 01:05-01:20 (no workflow, no agent):** one read-only look at the zink and pvr barrier code, one
  patch script, two builds, two A/Bs, one debug trace (≈30 tool calls, ~20 min). The trace answered "why no
  gain" in one run; an Opus/Fable review would have taken far longer and cost far more. Stopped as soon as the
  next step needed an intrusive change that could not be CTS-verified in the time box.

## RESUME HERE — Tuesday 2026-10-14 (session closed 2026-10-11 13:40 board clock)

- **Board state:** vendor `pvrsrvkm` + display-manager up, `schedutil`, `system.slice AllowedCPUs` cleared,
  crd-mirror running, `~/.dsh/boot-notify/pending.md` in place. Mesa `~/mesa/mesa-main` HEAD **`c737360`**
  (55 commits since `d253e35`), tree clean. Snapshots in `~/open-perf`: `vp` = HEAD, `bb` = `8c1ea61`, `h2` =
  `ddb1769`, `h1` = `14b0a9d`. Build dir = HEAD.
- **Published:** PR **https://github.com/ayiejosh/a733-powervr-fex/pull/11** (branch
  `open-driver-perf-2026-10-11`, one commit `c64e47d`: `mesa/open-driver-series/0044-0098`, regenerated
  `mesa/mesa-main-open-driver-all.patch` = `git diff 9a2b374..HEAD`, README "Added 2026-10-11", RESULTS rows).
  Pushed with the board's stored `ayiejosh` token; PR made via the API. Clone: board `~/a733-powervr-fex`.
- **Final numbers:** glmark2 off-screen (zink) **627** (vendor 930; 2026-10-10 evening 233). Last commits:
  uniform loops + register propagation: loop fragment-uniform=true 405 → 653. Final CTS (10 lists) identical to
  baseline; glvalidate 27/0. Backups: Mac `backup-2026-10-11/step2/` (patches 0001-0015 since `475930f`,
  `perf-1011e.bundle` = `9a2b374..c737360`, session.log, full glmark2 result).
- **Next (Tuesday):** (1) `ZINK_INLINE_UNIFORMS=1` on the loop scene (zink has uniform inlining, default off;
  it disables zink `optimal_keys` — measure both); (2) re-run the Vulkan A/B matrix (`~/open-perf/ovov.sh`)
  with HEAD — last matrix predates the last 12 commits; (3) worst glmark2 ratios: desktop blur/shadow (~30 %:
  per-pass GPU serialisation, frame = ~22 serial jobs), effect2d (50 %), shadow (46 %); (4) the separate task
  chip: pco hang on an unsupported 8-bit ALU op (`subgroups.vote.compute.subgroupallequal_i8vec2`);
  (5) Fable review idea not yet tried: an RA coalescing pass for vreg↔SSA copies (2 movs/iteration remain in
  loops with pred-exec ifs); the uniform-loop pre-pass could iterate to a fixpoint (counter incremented under a
  later uniform if in the same body is currently conservative).
- Rules unchanged: never push without the user's OK (given for this PR only); commit with the `ayiejosh`
  identity; board changes logged in `~/open-perf/session.log` with undo steps; no GPU benchmarks during CTS.

## Previous state (2026-10-11 ~11:40 board clock, Mesa HEAD `ddb1769`, tree clean; board on open, desktop down)

- **Committed 11:35:** `b65685c` igrp helpers skip empty blocks, `316b9a8` keep exec cond after CF lowering,
  `d06ae12` uniform-if branches + tstp, `95a5042` zero mrt desc (load-op cache + 6 CTS fixes), `ddb1769` EOT
  cache 32 entries. CTS `run-fx2.sh` identical on all 9 lists + xa_list; glvalidate 27/0. Snapshot `h2` = HEAD.
  Backup: Mac `backup-2026-10-11/step2/` (patches 0001-0011 since 475930f + bundle). NOT pushed.
  Next: buffer update-method=subdata scene (112 vs vendor 354).
- **subdata scene (board clock 11:45-12:30), uncommitted `bb`** (rogue/pvr_blit.c, pvr_cmd_buffer.h). GPU trace:
  ~53 transfer jobs/frame, transfer queue busy 62% of wall time, ~125 µs per kick (one kernel job + 3D kick per
  `pvr_transfer_cmd`; winsys asserts cmd_count==1). PVR_BATCH_DEBUG log: zink issues ~36 copies/frame of
  7-17 KiB from one u_upload staging buffer into 3 VBOs rotating over 4 backings (glBufferData orphan + subdata;
  the recycled backing is still in flight → zink stages + vkCmdCopyBuffer); each 16 KiB copy was 2 kicks
  (4 KiB rows + remainder row). Changes: (1) `pvr_try_batch_buffer_copy()`: 4-byte aligned copies ≤ 32 KiB
  between the same src/dst bases are rects (8 KiB rows, split at row boundaries of both) of ONE transfer cmd
  (`buffer_batch`, up to new `PVR_TRANSFER_MAX_CMD_MAPPINGS` 32 — cmd-source array only; the wa arrays stay 6),
  flagged FAST2D (clip blit: bg-loads the dst bounding box, since the PBE writes EVERY pixel of the clip box —
  first attempt without it corrupted ~1 MB), box ≤ 1 MiB, no overlapping dst rects within a batch, ≤ 4 MiB
  offsets. (2) plain path: copies ≤ 64 KiB use one rect (widest divisor ≤ 2048) instead of 4 KiB rows + remainder.
  Probe `~/open-perf/scat.c` (Mac scratchpad; scattered copies, byte-exact vs CPU model, mixed alignment,
  overlaps): all PASS on h2 and bb; 36×≤16 KiB: 7.16→1.73 ms. glmark2 subdata **100→203 FPS** (vendor 354),
  map variants/build/jellyfish unchanged; transfer jobs/frame 53→3, GPU busy 33% → CPU-bound now.
  **Committed `8c1ea61`** (CTS clean: new `cts/xfer_list.txt` 4634 buffer copy/update/fill/marker + memory
  mapping subset, caselist, robust, tfbc2, dr, ubo; glvalidate 27/0). Backup patch 0012 in step2/.
  Why the copies exist at all (tc instrumentation, `TC_MAP_DEBUG` prints + backtraces in u_threaded_context.c;
  all reverted): per frame glmark2 does draw → 48 glBufferSubData → glFinish. tc's `tc_improve_map_buffer_flags`
  can't infer UNSYNCHRONIZED: ranges are valid and the VBO is on the current batch's buffer list because the
  frame's draw (`u_vbuf_draw_vbo` → `tc_add_all_gfx_bindings_to_buffer_list`) is queued before the updates →
  `tc_buffer_map` stages via `u_upload` + queued `resource_copy_region` → vkCmdCopyBuffer. That is correct
  (the queued/in-flight draw reads the VBO); the driver-side busy callback says idle. A "lazy bind-list" tc
  change (don't add bound buffers to the list until the first draw) built and validated but changed nothing
  (the draw precedes the maps) → reverted. ZINK_DEBUG=norp: no change. Remaining per-frame cost is inherent to
  this pattern on our stack (3 transfer kicks ≈ 1.3 ms GPU incl. clip-blit bg loads; the vendor probably renames
  buffers with cached CPU memory, which pvr has no memory type for). main thread 31%/gdrv0 4% of a core,
  the hot libc symbol is memmove in the app's own vertex loop. Leave this scene (203 vs 354).
  Note: `libprof2.so` undercounts absolute CPU (per-thread CPUTIME timers deliver far fewer SIGPROFs than
  expected); use it for the distribution only, `/proc/PID/task/*/stat` for absolute CPU.
- **Full glmark2 off-screen on HEAD `8c1ea61` (board clock ~12:50): 627** (morning 429, vendor 930) →
  `~/open-perf/glm-open-off-bb.txt`, Mac `backup-2026-10-11/step2/glm-open-off-bb-627.txt`. Worst ratios:
  desktop shadow 273/905 (30%), desktop blur 103/321 (32%), loop fragment-uniform=true 458/1369 (33%; the
  constant-count loop runs 902 → our uniform-trip-count loop codegen is 2x slower than unrolled, vendor equal),
  shadow 254/553 (46%), effect2d 500/996 (50%), terrain 21/40. Best: texture mipmap 95%, buffer map 90%,
  gouraud 87%, build 86%. Next: the uniform loop FS (extend uniform-branch lowering to loops).
- **Uniform loops (board clock ~13:00), uncommitted `ul`** (pco_cf.c rewrite of the analysis + lowering,
  pco_internal.h fields `uniform_cond`/loop `uniform`,`analyzing`,`exit`; pco_ra.c empty-prologue fix). FS loop
  `for (i<FragmentLoops)`: body was 11 groups/iter (cndst/cndsm/cndend/cndlt + 2 mov pairs + 3 ALU) → 9 (tstp→p0,
  `br if(p0)` exit, 3 ALU + 2 movs, `br` back). A/B bb→ul: loop-uniform 397→502 (+26%), others ±3%. glvalidate
  27/0. CTS `run-ul.sh` running (cf2, pco, caselist, compute, robust, ubo, nullimg, tfbc). Commit msg drafted
  (`scratchpad/msgs/c7-uloop.txt`). Vendor runs this loop at unrolled speed (1369) → it must specialize on the
  uniform; next check Gallium "inlinable uniforms" in zink (would unroll the loop in NIR).
  CTS on `ul` FAILED: glsl.loops.special.while_uniform_iterations.nested_* (6), return_in_dynamic_loop_never,
  3 graphicsfuzz nested-loop cases. Two bugs: (1) pco_ra.c loop lifetime extension tested
  `end > loop_start_index`; with an empty prologue the loop starts at the body's first instr where the counter's
  last read is → not extended → counter clobbered (fixed: `>=`); (2) marking condition producers (ignorepe) is
  WRONG when a producer (loop counter increment) sits in a loop that is finally divergent: instances that
  already left the loop keep incrementing. The marking was only insurance for instances masked at entry; the emc
  init at entry (`cndst ALWAYS`) makes all instances active, so it is unnecessary → removed (uniform ifs rely on
  masking_nestings==0 ⇒ all instances active, p0 from an E1_ZX test is valid for all). After both: the 10 cases
  pass, cf2 identical. Also built `vp` = ul + register-read propagation: `load_reg` → `pco_mov`; pco_opt fwd-prop
  propagates a vreg source only within the block with no write to it in between (`vreg_unchanged_between`);
  `cmp_src_is_stable()` accepts a vreg unwritten from the bcmp to the end of its block when the if follows
  that block. CTS `run-vp.sh` running (9 lists).
  `vp` results: loop body 6 groups/iter (tstp, br exit, fmul, fmad, imadd, br back — no copies); A/B bb→vp:
  loop-uniform 401→640-663 (+62%), bump normals +3-5%, others ±3%; glvalidate 27/0. Commit messages drafted:
  `scratchpad/msgs/c7-uloop.txt` (ul: pco_internal.h, pco_ra.c, pco_cf.c = /tmp/pco_cf.ul.c + nomark) and
  `c8-vprop.txt` (vp: pco_cf.c final, pco_opt.c, pco_trans_nir.c). A second Fable review of the diff was spawned.
  WIP diff backed up: Mac `backup-2026-10-11/step2/wip-uloop-vprop.diff`.
  Second Fable review: A1 `pco_group_instrs.c find_sink` sank a vreg read past a vreg write (fixed with
  `depends_on(b, user)`); A2 RA inverted range for a vreg first written in a loop (fixed: extend to the loop);
  A3 exponential `ref_is_uniform` (visit cap 4096); dead `producers` list dropped. All 10 earlier failures pass,
  cf2/xa identical, loops_list (glsl loops + spirv opphi/loop_control) identical to bb (1 pre-existing fail
  `opphi.wide`). **Committed:** `f0067ac` uniform loops, `a91aab5` RA inverted range, `c737360` vreg
  propagation + find_sink. HEAD `c737360` = 55 commits since `d253e35`. Final CTS `run-vp4.sh` (10 lists).
- **PR prep (user: "push the commit to the repo as PR", "do new PR for this").** Target repo found via the board's
  stored GitHub credential (login `ayiejosh`): `ayiejosh/a733-powervr-fex`, default branch `trixie`, Mesa work
  carried as patches under `mesa/open-driver-series/0001-0043` (ends at `d253e35`; base `9a2b374`) + combined
  `mesa/mesa-main-open-driver-all.patch`. Clone at board `~/a733-powervr-fex`, branch
  `open-driver-perf-2026-10-11`, commit `c64e47d`: patches 0044-0098 (`format-patch --start-number 44
  d253e35..HEAD`), combined patch = `git diff 9a2b374..HEAD`, README "Added 2026-10-11" section, RESULTS rows.
  PR body in Mac `scratchpad/pr/body.md`. Push + PR (API) after CTS + final glvalidate.
  Zink uniform inlining exists but is off by default (`ZINK_INLINE_UNIFORMS`, default on only for CPU drivers;
  it disables `optimal_keys`). To test on the loop scene after CTS.

- **Step 2 continued (board clock 09:30-10:30).** `14b0a9d` = the `nt` null-test change (CTS clean).
  `aad4287` colour write enable: zink makes CWE dynamic in every pipeline, so every GL FS read po0 (olchk) +
  bcsel and the pipeline was TRANSLUCENT (no HSR). Now CWE is lowered after packing (`lower_color_write_enable`
  in pco_nir_pvfio.c, on `state->stores`): `if (!(fs_meta & bit)) value = old packed po` → predicated, no
  select; `pco_fs_data.cwe_meta_mask` = bits checked; driver `pvr_fs_cwe_reads_fb()` makes the draw
  TRANSLUCENT only when one is clear; `uses.fbfetch_static` = fb reads outside CWE. Alpha-to-one lowered only
  when `meta_present.alpha_to_one` (static enable or dynamic). ISP re-emit on CB_COLOR_WRITE_ENABLES /
  CB_LOGIC_OP_ENABLE dirty; fs_meta re-upload on MS a2o/a2c/sample mask/samples dirty. vs h1: function +9%,
  conditionals +10%, effect2d +11%, jellyfish +9%. CTS identical (new `cts/cwe2_list.txt` 2944: CWE 600 pass,
  logic_op 192, EDS 37; caselist, pco, tfbc2, dr, robust); validate 27/0.
  Uncommitted `ub` (pco_cf.c, pco_internal.h): uniform ifs (cond built only from shared/const/imm by pure ALU
  ops, no masking loop/cond-exec ancestor) lower to `tstz p0 (ignorepe)` + `br if(p0)` (+ `br` ignorepe in the
  interlogue) instead of cndst/cndef/cndend; producers set to ignorepe so p0 is valid in all instances; big
  uniform pred-exec bodies (>4 instrs) become branches; emc now initialized at function entry (first version
  initialized it inside a skipped body → loop scene 406→34 FPS; fixed). vs cw: function +4-6%, conditionals
  +3-9%, jellyfish +7%, loop +4-6%. CTS `cts/run-ub.sh` running (new `cts/cf_list.txt` 2253: glsl loops/switch/
  functions/return/discard/demote, subgroups vote/ballot_other/basic, spirv control flow; baseline on cw).
  Branch semantics used: `br` exec_cond = taken if any instance passes (loop back-edge relies on it);
  allinst/anyinst from vote lowering. Next idea: let bcmp write p0 directly (saves the tstz per if).
  run-ub.sh was stopped: `subgroups.vote.compute.subgroupallequal_i8vec2` hangs in pco_trans_nir (pre-existing:
  `Unsupported alu instruction` + UNREACHABLE loops in release) — `cts/cf2_list.txt` (2130) drops 8-bit cases;
  spawned a separate task for it. Added `tstp` (new op = bcmp writing a predicate, map copied from tstz):
  `set_p0_from_cond()` in pco_cf.c recomputes a bcmp condition into p0 in every if prologue (pred/cond/uniform),
  flipping invert_cond; DCE drops the unused bcmp. Snapshot `tp` = ub + tstp: function FS prologue 2 groups
  (was 8 in h1). vs ub: conditionals +3-5%, loop +4-8%, effect2d/function parity. Running `cts/run-tp.sh`
  (cf2 identical to cw: 1294 pass / 1 fail).
  run-tp.sh result: pco/caselist/ubo/nullimg/tfbc2/dr identical, but robust 67 Crash (compute buffer_access
  cases) + compute `device_group.device_index` Crash: segfault in pco_group_instrs → `calc_align_padding()` →
  `pco_prev_igrp()` stepped into the EMPTY epilogue block of a trailing uniform if (cond-exec epilogues always
  had a cndend). Root fix in pco_internal.h: `pco_prev_igrp`/`pco_next_igrp` use `pco_{prev,next}_block_nonempty`
  (as `pco_cf_node_offset` already does). Both cases pass; rerun `cts/run-tp2.sh` (robust, compute, cf2, pco,
  caselist) → all identical to baseline; glvalidate 27/0, kernel clean. Fable review agent asked (advisor tool
  timed out) — commit of ub+tstp waits for it.
  **Full glmark2 off-screen on `tp`: 560** (morning 429, vendor 930) → `~/open-perf/glm-open-off-tp.txt`. Worst
  ratios: desktop blur 78/321 and shadow 221/905 (24%), loop uniform 414/1369 (30%), buffer map 112/354 (32%),
  shadow 218/553 (39%), effect2d 465/996 (47%); best build 1026/1185, texture 1160/1311 (87-88%).
  (The slow buffer scene is update-method=**subdata** 112/354; map variants are 307/364 and 347/450.)
- **Desktop scene diagnosis (board clock ~10:40-11:10).** CPU 35% of a core, GPU job trace
  (`/tmp/gputrace.sh ICD SCENE OUT` + `/tmp/gtparse.py`, `/tmp/gttime.py`) showed no job in flight 51% of the
  time. Per-thread sampler `~/open-perf/libprof2.so` (Mac `scratchpad/prof/prof2.c`: per-thread
  CLOCK_THREAD_CPUTIME_ID timers via a pthread_create wrapper — Mesa threads block all signals; dlclose is a
  no-op so ICD symbols resolve; timestamps; `scratchpad/prof/symwin.py PROF THREAD FROM TO` windows + buckets):
  `gdrv0` spent 48% of steady state in nir/pco. gdb on `pco_trans_nir`: `pvr_uscgen_loadop` from
  `pvr_mrt_load_op_state_create` (CmdBeginRendering) and `pvr_usc_eot` (CmdEndRendering), every render pass.
  Causes: (1) `pvr_arch_init_mrt_desc` left `priority` + unused `valid_mask` words uninitialised → load-op cache
  key (memcmp, 16 entries, no eviction) never matched; priority also steers output-reg allocation → fixing it
  makes 6 old CTS fails pass (renderpass{1,2} attachment.8.797, attachment_allocation.shrink.66). (2) EOT cache
  was ONE entry keyed by PBE words (= target address) → 32 entries round-robin. Snapshot `ec`: desktop blur
  67→95, desktop shadow 206→260, shadow 205→240, effect2d 434→490; gdrv0 CPU ~5.7x lower. CTS `run-ec.sh`
  clean (dr, rp_extra 414/0, tfbc2, caselist, pco, tfbc 3993/3). Remaining desktop: CPU submits a frame in
  0.3 ms; GPU runs ~22 jobs serially (frag 0.4/1.0 ms alternating, geom 0.11 ms waiting on the previous frag).
- **Fable review** (agent; report in this session): A1 — post-cf passes (`pco_const_imms` movi32→literal mov,
  `pco_opt` mod/vec lowering) rebuilt instructions with default exec → (ignorepe) producers and predicated
  instructions lost their condition. Fixed with `pco_instr_copy_exec_cnd()` (pco_builder.h) at all 6 sites.
  A3 — accept already-(ignorepe) producers in `ref_is_uniform`. Its idea #1 (defer wdf) was already tried
  post-RA with no gain. Snapshot `fx2` = ec + these; CTS `run-fx2.sh` running incl. `cts/xa_list.txt`
  (graphicsfuzz, fragment_operations, 1/3 glsl.derivate, memory_model atomics sample). Commit messages drafted
  in Mac `scratchpad/msgs/c1-igrp.txt .. c5-eot.txt`.

- **2026-10-11 07:30- user: "1 then 2" (1 = TFBC render-pass fixes + widening, 2 = compiler).**
  Step 1 committed: `5fd7b28` render-pass TFBC fixes (DONT_CARE attachments that may be TFBC get a render init
  entry; normal load op only if an entry initializes something; bg tag / empty tiles count only initialized
  entries; dynamic rendering promotes DONT_CARE/NONE→LOAD for TFBC + partial area; `pvr_clear_attachments`
  render-init clears used hw render 0 instead of `sub_cmd->hw_render_idx` (the NextSubpass crash);
  `pvr_loadops_contain_clear` OOB loop). `46c46b3` TFBC only for images ≥ 8x8 padded extent (5x1 → 8x1 = 32 B,
  block slot 256 B overran — latent bug, found via draw.renderpass.basic_draw.misc.flat_b_sat_error).
  `dd17f33` TFBC widening. CTS identical on tfbc, tfbc2, new `cts/rp_extra.txt` (1446 render pass
  load_store_op_none/attachment_allocation/render_area), dr, 5062, pco, sync, robust. glmark2: build 751→913,
  texture 804→967, phong +8%, effect2d +6%.
  Step 2: `40486fe` driver-side descriptor checks (pco_desc_check, `load_desc_checks_pco` intrinsic, shared reg,
  `PVR_BUFFER_TYPE_DESC_CHECKS`, per-draw upload) — only for versioned shaders WITHOUT texture/image null tests:
  with them NIR folds the per-access null tests inside the checked copy (→ zeros); an opaque flag keeps both copies
  (effect2d FS 72→166 groups, 32→52 temps, −7..10%). Upper bound (slow=false) was effect2d +24%, conditionals +27%.
  Uncommitted `nt`: `trans_is_null_desc` tests only texaddr (dwords 2-3) for combined image samplers (effect2d
  +4..9%, jellyfish +7%); CTS `cts/run-nt.sh` running. Noted: DITR post-fence (flush + 2 br.next) per varying
  fetch is upstream "WDF ordering" legalization — left alone.

- **Final state 2026-10-11 ~07:00.** Commits `805fddd..475930f` (backup: `backup-2026-10-11/` patches + bundle +
  session.log). glmark2-es2 off-screen through zink: **233 → 429** (vendor native GLES 930; vendor zink off-screen
  is broken at 12). Vulkan matrix `~/open-perf/ovov-1011-final.txt` (open/vendor medians, 3 alternating rounds):
  r512 0.358/0.358, r2048 1.766/1.969 (+11.5%), b512 0.637/0.608 (−4.6%), b2048 5.358/5.321, c512 0.237/0.242,
  c2048 2.530/2.529, vkheavy 171.7/179.8 (+4.7%), cstpf 148.6/146.2, cstpi 151.6/147.1, cstp 447.9/372.5 (+20%),
  vkmany512 0.817/0.801 (−2%), vkmany2048 5.729/5.615 (−2%).
  Windowed glmark2 (weston+Xwayland) ~105-134 FPS vs ~800 off-screen: presentation/compositing dominates
  (Xwayland CPU, user's FEX/crd-mirror services share CPUs) — not investigated.
  **Next levers:** (1) TFBC for SAMPLED/TRANSFER_DST/INPUT_ATTACHMENT/MUTABLE(UNORM+SRGB) colour targets (+5-12%
  GL off-screen; also Vulkan render-to-texture bandwidth) — needs render-pass fixes first: DONT_CARE attachments
  absent from `color_init` (pvr_arch_hw_pass.c ~437) so the force-load variant can't preserve pixels outside a
  partial render area; and `pvr_perform_start_of_render_attachment_clear()` passes a `color_init` index as
  `VkClearAttachment.colorAttachment` (subpass index) → segfault in `pvr_clear_attachments` from
  `CmdNextSubpass2` (renderpass*.suballocation.attachment_allocation.*). Dynamic rendering: promote DONT_CARE→LOAD
  for TFBC + partial area at BeginRendering. (2) FS code quality (effect2d FS 72 instruction groups; FS-heavy
  scenes ~2.5× behind vendor GLES). (3) b512/vkmany host latency (−2..5%).

- **2026-10-11 00:50-02:50 — GL through zink (glmark2), user asked for "much better than vendor".**
  Off-screen glmark2 (`~/open-perf/glbench.sh ICD OUT --off-screen ...`, weston+Xwayland, client
  `CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink"`): open 233 vs vendor native GLES 930 (vendor zink off-screen 12,
  broken). Gap is ~4x on every scene, even trivial ones (texture 454 vs 1311 FPS).
  1. **RT dataset cache** (uncommitted; `pvr_rt_dataset.c/.h`, `pvr_device.h`, `pvr_arch_device.c`,
     `pvr_arch_job_render.c`): zink uses dynamic rendering, so pvr built a render-target dataset per render. Now an
     LRU pool of 4 keyed like the dataset create args. glmark2 +13%.
  2. **Robust/null descriptor check versioning** (uncommitted; `pco_nir_lower_null_descriptors.c`, `pco_nir.c`,
     `pco_internal.h`): zink always enables robustBufferAccess + nullDescriptor, so every UBO load got a bounds
     check and every texture sample a null branch. New `pco_nir_version_descriptor_checks()` tests once at shader
     start (null texture/image descriptor, or UBO size < max constant offset read) and runs an unchecked copy of the
     shader when all pass, else the original checked copy; output stores merged after the if via phis.
     Buffer null checks stay on (an earlier "skip buffer null checks when robust" build `nb2` broke 77 robustness2
     null_descriptor cases: the robust clamp reads address 0 of a null buffer).
     Bugs fixed on the way: UBO offsets are `desc.z + const` (unfolded) at this point → `ubo_load_extent()` with
     `nir_scalar_resolved` + `nir_opt_constant_folding` before the pass; `remat_def` used `nir_src_rewrite` on
     uninserted clones (segfault in robustness2 ...uniform_buffer_dynamic.null_descriptor.1d.vert).
     A/B ×2 vs previous build (`icd-nv.json`), FPS build/texture/phong/jellyfish/effect2d/terrain:
     nv 303/589/154/101/239/8, 298/555/149/98/227/8; new 368/537/206/136/232/11, 360/544/209/139/240/10.
     Library `libvulkan_powervr_mesa.uv.so` / `icd-uv.json`; CTS `cts/run-uv.sh`: caselist 4439/623, pco
     4539/1842/47F (47 pre-existing ssbo.layout), compute 678/557, tfbc 3989/5174/7F (7 pre-existing), sync 902/816
     all identical to `nv`; robust 640 P / 8 F / 2 Crash (head 648/0/2, crashes pre-existing).
     **The 8 new fails** = robustness2 storage_image `no_fmt_qual` null_descriptor frag (cube/cube_array/2d_array,
     unroll). Cause: versioned FS needs > max temps → RA spills 1 temp, and **spilling is broken upstream**: the
     shared regs for the spill buffer address (`spill_info`) are only reserved in `pvr_postprocess_shader_data()`
     when `spilled_temps` is already known, i.e. never on a first compile (`PCO_DEBUG=ra_force_spill` aborts even
     without versioning). Fix (uncommitted, `pvr_arch_pipeline.c`): keep each stage's pre-lowering NIR + pco_data;
     if the compiled shader spilled and `common.checks_versioned`, re-lower with `common.no_check_versioning` and
     recompile (graphics helper `pvr_graphics_lower_stage()`, compute loop; mrt_setup destroy moved after the trans
     loop). Failing case now 0 spills / 211 temps / Pass. Library `uv2`; CTS `cts/run-uv2.sh`.
  3. Sub-allocator 4-slot cache (`sc`): **no gain, reverted** — the per-frame CREATE_BO was not the
     sub-allocator. gdb on `pvr_drm_winsys_buffer_create` showed `pvr_spm_init_bgobj_state` (per render).
  4. **COMMITTED locally 2026-10-11 ~04:15 (not pushed):** `805fddd` RT dataset cache; `e218995` descriptor check
     versioning + recompile-on-spill; `54adf97` load-op program cache (every BeginRendering compiled the load-op
     USC shader with full pco — found with gdb stack sampling, 6/11 busy samples of zink's `gdrv0`); `2dca745`
     SPM bgobj consts from the general sub-allocator (removed the per-frame CREATE_BO/VM_MAP/VM_UNMAP/GEM_CLOSE).
     CTS on this state (`c1`): robust, 5062, pco 6428, compute 1235, tfbc 9170, sync 1718, new `cts/dr_list.txt`
     (6745 dynamic_rendering sample, baseline `wgm2`): all identical to baseline.
     glmark2 FPS (build/texture/phong/jellyfish/effect2d/terrain), morning `nv` 298/555/149/98/227/8 → `sp`(=c1)
     447/724/228/142/252/11.
  5. **UNCOMMITTED (`c3`, CTS `cts/run-c3.sh` running):** SPM EOT program cache (EOT USC program was also
     compiled per render) + TFBC widening (`pvr_image.c/.h`: usage may add SAMPLED/INPUT_ATTACHMENT/TRANSFER_DST,
     MUTABLE_FORMAT allowed when the VkImageFormatListCreateInfo only differs in sRGB-ness, `may_fbc` adds the
     RGBA8/BGRA8 SRGB formats). zink's colour target was usage 0x97 + MUTABLE (list {RGBA8_UNORM, RGBA8_SRGB}) →
     never compressed. Measured with an env hack: texture +12%, build +5-10%. New list `cts/tfbc2_list.txt` (4223
     copy_and_blit/image_clearing/texture/renderpass2 on RGBA8/BGRA8 UNORM/SRGB), baseline run on `c1`.
     **Result:** tfbc2 list identical (2036 P) but the 9170 TFBC list regressed: 87 F + 12 Crash
     (renderpass1/2.suballocation.attachment[_allocation], draw.renderpass.basic_draw.misc.flat_b_sat_error);
     all pass with `PVR_DEBUG=nofbc`. Cause found for the fails: partial render area + `loadOp DONT_CARE` on an
     image that is now TFBC (CTS images carry TRANSFER_DST): the edge 8x8 blocks overwrite pixels outside the
     render area. The force-load variant (`load_op_state_fbc`) only covers `hw_render->color_init` entries, and
     `pvr_arch_hw_pass.c` (~line 437) never puts DONT_CARE attachments in `color_init`. Crashes not analysed.
     **TFBC widening dropped** (kept as `/tmp/pvr_image.{c,h}.fbcwide` on the board). Re-do needs: DONT_CARE
     attachments of possibly-TFBC formats in `color_init` (op DONT_CARE: normal variant ignores, force-load variant
     loads), then the crash cases. Same gap exists today for COLOR_ATTACHMENT|TRANSFER_SRC images (untested by CTS).
     `c2` = c1 + EOT cache only; CTS `cts/run-c2.sh`.
  6. **More local commits (2026-10-11 ~05:00-06:10):** `859daf6` SPM EOT program cache (c2 CTS identical,
     glmark2 --validate 27/0 — note `glvalidate.sh` now takes `CLIENT_ENV`; without the zink override it shows the
     old kmsro 0/8 + "vkEndCommandBuffer OUT_OF_DEVICE_MEMORY" artefact); `23a753d` SPM scratch buffer reused when
     larger (store only reused on exact size → shadow scene freed/allocated a framebuffer-sized BO ~2×/frame,
     3.6 ms each: shadow 64→149); `8928e2b` RT dataset cache 4→16 (desktop blur 51→62); `10b3f7d` free binding
     arrays of a spilled compile attempt (leak in the e218995 retry path). Full glmark2 off-screen on c2: **359**
     (morning 233, vendor native GLES 930). Host time per frame now ~170 µs (sched trace, texture).
  7. **UBO preload (uncommitted, `up`, CTS `cts/run-up.sh` running):** experiment first — glmark2 `--data-path` copy
     with 2 of 3 phong VS matrices as constants (same ALU): 218→291 FPS ⇒ per-vertex UBO loads dominate the geometry
     phase (phong TA 2.54 ms vs vendor's whole frame 1.26 ms). pco gathers constant-offset load_ubo ranges
     (≤64 dw/UBO, ≤128 dw, ≤4 UBOs), pvr allocates shareds and adds `PVR_BUFFER_TYPE_UBO_PRELOAD` DOUTDs to the
     descriptor upload program; draw time reads the descriptor from the bound set's CPU mapping (dynamic: set
     `dynamic_buffers[]`, +offset), zero page if null/too small. UPDATE_AFTER_BIND/PARTIALLY_BOUND excluded
     (`pco_binding_data.no_preload`). glmark2: build 494→740, phong 237→424, gouraud 368→662, bump high-poly
     145→288, terrain 12→18, jellyfish 146→171. `--validate` 27/0. New CTS list `cts/ubo_list.txt` (4773).
     **COMMITTED** `e903693` (CTS: ubo 4773, robust, 5062, pco 6428, compute, dr 6745, tfbc, sync all identical;
     --validate 27/0) and `475930f` limits 256/256 (jellyfish 167→181; ubo/robust/5062/pco identical).
     **glmark2 off-screen score 429** (morning 233; vendor native GLES 930).
  - Rejected: skipping the depth store (+10% but GL semantics need it). FW TA→3D gap ~200 µs is FW-internal.
  - Tried, no effect, reverted: vectorizer robust_modes off after robust lowering (fast path already vectorized);
    post-RA `pco_sink_wdfs` (moving `wdf` past instructions not touching the smp/ld dest regs; only 2 of 9
    effect2d samples overlapped since each result is used right away, and no FPS change); `nir_schedule` in
    pco_postprocess_nir (crashes in `pco_const_imms` `blocks_src_swap` — not debugged); sampling 2D textures
    as 3D under image2DViewOf3D (zink enables it) costs nothing measurable. Texture-heavy FS scenes
    (effect2d 360 vs 996) look issue-bound: effect2d FS = 72 instruction groups → compiler code quality.
  - Host side: main thread spends ~50% of frame waiting for zink to submit; sched trace (contaminated by a
    concurrent CTS run — my mistake) shows `gdrv0` ~410 µs busy per frame. gdb stack sampling is biased (said
    idle). New tools: `~/open-perf/libprof.so` (LD_PRELOAD SIGPROF sampler, `PROF_OUT=file PROF_US=250`, symbolize
    with Mac `scratchpad/prof/sym.py`), `/tmp/schedtrace.sh ICD SCENE` + `scratchpad/schedan.py`.
  - `linux-perf` apt install failed (board apt has mixed Ubuntu/Debian deps); nothing installed.

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

## 2026-10-14: sync-type bug fix, PR #11 updated to commits 44-105

- Bug: patch 0018 (48ce5be) registered the emulated timeline type next to the native syncobj type. With kernel timeline syncobj both advertise TIMELINE: debug builds assert (vk_sync_type_validate type->init), release builds mixed the types. First fix (fa5832f) left ws->timeline_syncobj_type uninitialised, but pvr_queue_init still created queue->job_sync[] from it (unused code), so the assert build still failed. Final fix dcd4d26: register the emulated type only without native timeline, delete the unused job_sync[]/job_value[].
- Evidence: syncset 5637 cases, build without fix vs fix: 596 diffs, all timeline_semaphore (109 Crash + 486 DeviceLost -> Pass, 1 DeviceLost -> Fail). Assert build: 1235/1235 syncset passes, foldtest/nantest/validate clean. The one Fail (device_host write_ssbo_compute_indirect_read_vertex_input.buffer_262144) passed 60/60 on release and 60/60 on assert when run alone; unexplained single failure under 4-worker load. Binary-semaphore cross_instance write_ssbo_fragment_read_vertex_input.buffer_262144 fails on both builds (pre-existing).
- Speed A/B no-fix vs fix: within noise off-screen and windowed (all scenes within 3 %). Final full glmark2: 690 / 687 (vendor 930), geomean vs vendor 0.719 / 0.715.
- PR: ayiejosh/a733-powervr-fex #11, single commit, c64e47d -> 8208cdc (push --force-with-lease), title/body updated. Patches 0099-0105 in backup-2026-10-14/.
- Process: session14/16 took 5.5 h because gate.py rechecks every differing case 3x on both builds; the base build crashed on all 596, one pass took 78 min. Killed by PID. Lesson in memory long-job-hygiene.md.
- Not re-run on the last tree: the 15-list CTS gate (0105 does not touch compiler/render paths). Debt: ~30 debug getenv trace hooks in the branch diff.
