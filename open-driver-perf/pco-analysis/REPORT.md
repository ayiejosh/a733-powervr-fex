# pco codegen improvements for real fragment shaders — ranked candidates

Date: 2026-11-01 (round-210)
Tree: `/home/radxa/mesa/mesa-main` @ `6e9baf84`, clean. All analysis is static; no board
runs, no GPU workloads, no writes to the Mesa tree.
Inputs: pco sources listed below + pre-existing IR dumps
(`/tmp/pco-math3.txt`, `/tmp/ilp-ir.log`, `/home/radxa/open-perf/pco-*.txt`).

Measured problem being attacked (from session rounds 194-209):
- db2 `math` (Phong-style highp FS): vendor 1292 fps, native 1032 fps (**0.80x**).
- db2 `ilp` (64 independent varied-multiplier FMULs): vendor 434, native 381 (**0.87x**).
- db2 `trans`, serial FMA chain, ALU/fill microbenchmarks: **parity**.
- glmark2 shading/bump/effect2d: 0.45-0.69x vendor; round 201 proved shading = 1 draw/1 FS
  pass, so the residual is fragment-shader issue efficiency, not extra passes.
- Round 198/208: `no_dual_issue` = -25%/-12% on `math`, so the dual-issue machinery does
  real work but is not extracting everything; skip knobs (`pco_schedule`, window 4/16) are
  neutral or worse.

## Instruction-mix / dual-issue evidence (from saved dumps)

Parsed from the local IR dumps (`shader ir after passes` sections; `p1:` = co-issued phase 1):

| shader | source | groups | main-ALU groups | dual-issued groups | pairable p0 ops left unpaired | literal (`bbyp0bm_imm32`) groups | temps |
|---|---|---|---|---|---|---|---|
| db2 `math` FS (realistic Phong) | `/tmp/pco-math3.txt` (Nov 1, 6e9baf84) | 26 | 17 | 5 (29%) | 4 | 4 (15%) | 8 |
| db2 `ilp` FS (per-iteration literals) | `/tmp/ilp-ir.log` (older build) | 148 | 85 | **0** | 84 | **59 (40%)** | 10 |
| vkheavy heavy FS (pre-dual-issue) | `/home/radxa/open-perf/pco-vkheavy.txt` | 944 | - | n/a (v1) | - | 7 | 10 |

Two facts fall out immediately:

1. **Literal materialisation groups are dual-issue barriers.** `bbyp0bm_imm32` is a *bitwise*
   ALU group (`pco_ops.py:460-461`, `pco_isa.py:2790-2828`: imm16/imm32 live in the bitwise phase-0
   encoding, main-ALU phases have no immediate fields). `try_dual_issue()` stops scanning at the
   first instruction for which `is_plain_alu_op()` is false (`pco_group_instrs.c:210-234, 425-427`),
   which includes every literal group. In the `ilp` dump every arithmetic op is separated from its
   neighbours by one literal group, so the scan never reaches a candidate: **zero pairs in 148
   groups**. In the `math` dump 4 of 26 groups are literal groups and they sit exactly in the
   pairing windows (e.g. groups 0007-0009 then 0010; 0023 between 0022 and 0024).
2. The `math` FS is otherwise close to using the hardware: 5 of 17 main groups already dual-issue
   and the remaining unpaired pairable ops are mostly blocked by real dependencies or by the
   literal barriers above. The heavy Vulkan FS (built by the reference driver) has only 7 literal
   groups in 944 because the **Vulkan driver enables pco's literal pool**
   (`pvr_arch_pipeline.c:2943`); the native gallium FS path does not (see candidate 1).

Estimate for the `math` FS: removing 4 literal groups is -15% issue slots and unblocks ~2-3 more
pairs (-8%); combined -20%..-23% groups, in the same range as the measured 0.80x. Group count is
not wall time (transcendental latency, memory), so expect a fraction of that until measured.

## Ranked candidates

### 1. Enable the pco literal pool for native gallium graphics shaders (FS/VS) — driver-only
**Where:** `src/gallium/drivers/pvr_native/pvr_shader.c:672` (`pvr_shader_compile_data`),
`pvr_shader.c:341-367` (`shared_dma_start` / `shared_dma_dwords`), `pvr_shader.h:126-140`,
`pvr_draw.c:994-1050` (`draw_uniform_program`, the per-draw shared-register DMA fill).
**What:** `data->common.literal_pool = true` is only set for compute
(`pvr_shader.c:817`, and in the Vulkan driver at `pvr_arch_pipeline.c:2943`). The pco side is
already complete and measured (`pco_const_imms.c:264-331`, commit `c1012110`): up to 32 literals
that are not in the hardware constant table (`const_reg_defs`, `pco_const_imms.c:32-129`) are
placed in shared registers after the driver-allocated ones and read directly by instructions
(shared registers are legal in hardware sources S0, S2, S3; `shared_readable_ssa()` decides per
use and the legalizer can swap the first two sources of fadd/fmul/fmad).
Plumbing for graphics: after `pvr_shader_compile_data()` returns, copy
`data.common.literals` / `literal_vals[]` into `pvr_shader`; include the literal range in
`shared_dma_start` (currently `= data.common.shareds`, so with literals it would start *after*
them - must `MIN2` with `literals.start`); in `draw_uniform_program()` write
`literal_vals[]` into the zeroed `u[]` block at `(literals.start - shared_dma_start)` exactly
like blend constants / texture descriptors. Precedent in-tree: `pvr_compute.c:240-250,391-398`
and the Vulkan path `pvr_arch_pipeline.c:641-651`.
**Why:** each pooled literal removes one instruction group (a whole issue slot) and one temp);
`math` FS: 4 groups = 15% + pairing unblock + temps 8 -> ~5. Measured on the reference Vulkan
driver (`c1012110`): cstpf 119->115 groups, cstpi 119->115, vkheavy temps 11->8.
**Risk:** low. Pooling is usage-checked (only pooled if every use can read S0/S2/S3), stops at
`max_coeffs`, existing mechanism, output hashes unchanged in the c1012110 testing. Costs a few
dwords of per-draw DMA and shared registers (FS already uses coeffs for varyings - watch capacity
on shaders with many varyings).
**Measure:** `PCO_DEBUG_PRINT=fs,stats` group/temps count on db2 `math` and glmark2 shading;
then interleaved A/B fps (db2 math,.glm shading/bump/effect2d). Expected: math groups 26 -> 22,
temps 8 -> 5.

### 2. Let the dual-issue window cross single-instruction bitwise groups
**Where:** `pco_group_instrs.c:210-234` (`is_plain_alu_op`), `:396-428` (`try_dual_issue`
scan/barrier), pre-RA sibling `:561-582` (`find_hoist`) and `:585-614` (`find_sink`).
**What:** `try_dual_issue()` refuses to scan past anything that is not a "plain ALU" op
(`:425-427`), so a `bbyp0bm_imm32` / `bbyp0s1` / `pck` / `msk_*` / `logical.*` group (all bitwise
ALU, pure, single group) is a hard barrier. Treat pure bitwise single-instruction groups as
crossable "between" instructions: hoisting a p1 candidate above them only requires the existing
`depends_on()` WAR/WAW/RAW checks (`:246-261`), which are complete on hardware regs post-RA.
Keep control-flow ops, DRC/WDF/IDF, memory/tex ops and anything with side effects as barriers
(they already are - don't add them to the crossable set). Apply the same change to
`is_plain_alu_op()`'s use in the pre-RA scheduler so it creates the adjacency early; on SSA the
`reads_def_of()`/`refs_overlap()` checks are sound (an SSA use cannot precede its def in the same
block).
**Why:** the p1 candidate is independent of a literal materialisation by construction (a literal
group defines a temp the candidate may read - `depends_on` catches the RAW if it does). This is
the change that makes the `ilp`-style code dual-issue at all; on the `math` FS it frees the
windows around groups 0007-0009 and 0023. Expected: 1-3 extra pairs on `math`, potentially large
(10-30% fewer groups) on literal-heavy real shaders (glmark2 bump/effect2d/shading material
constants).
**Risk:** low-medium. Ordering changes; all dependencies are checked against hardware registers.
Group size can grow because a paired group may need a wider upper-source variant
(`pco_map.h.py:84-89`, `I_SRC` specs in `pco_isa.py:405-440`) - `calc_lengths()` accounts for it,
but watch code size/icache. Must keep `PCO_DEBUG=no_dual_issue` covering it.
**Measure:** group + pair count via IR dump on `math`/`ilp`/real glmark2 FS; fps A/B; CTS gate.

### 3. Post-RA placement pass for dual issue ("schedule after RA", the `pco_ir.c:52` TODO applied to pairing)
**Where:** new pass between `pco_post_ra_legalize` and `pco_end` in `pco_ir.c:57-60`; reuse the
helpers in `pco_group_instrs.c:236-361` (`refs_overlap`/`depends_on`/`upper_src_ok`/
`p1_srcs_ok`/`dual_dest_ok`/`hdr_compatible`/`swap_commutative_srcs`) and the SSA scheduler
structure of `pco_dual_issue_sched` (`:622-662`).
**What:** today the only reordering for pairing happens **before RA** (`pco_dual_issue_sched`,
`:622`) and `try_dual_issue` (`:374`) can only pull a candidate *forward within 8 plain-ALU
instructions*. What is lost by scheduling before RA:
- `pco_ra` (`pco_ra.c:909-1129`) inserts MOVs for vec components (`emit_copies`) and drops
  no-op `mbyp`s; `pco_post_ra_legalize` additionally inserts MOVs via `insert_mov_ref()`
  (`pco_legalize.c:81-120`) after RA. Any of these can land between a pre-RA-placed pair and
  break the adjacency; a plain-ALU scan can still see through some, but not through a new
  move whose source is a special register *before* the candidate was placed.
- RA coalesces vecs and reuses temps; two SSA values that were independent pre-RA can become the
  same hardware register, destroying a scheduled pair (or vice versa, RA's register assignment is
  what decides `p1_srcs_ok`: upper sources sA accept 3 bank bits/11 index bits, sB/sC only 2/8, so
  shared (sc) and indexed registers are only encodable in p1 source 0; the pre-RA scheduler
  cannot know whether a shared literal lands in s1/s2, it only knows the swap will fix fadd/fmul).
- Pre-RA placement can only look at SSA order; post-RA the actual first use/first def is final.
A minimal post-RA pass: walk each block; for every pairable phase-0 instruction A (single dest,
default exec, op in `is_p0_pairable`), search +/-8 instructions for a p1 candidate B
(`fadd/fmul/fmad/mbyp`, single dest, default exec, `hdr_compatible` with A, no data dependence
between A and B, `p1_srcs_ok` after optional commutative swap, dest compatible with A's dest);
move B to immediately after A, checking `depends_on()` against every crossed instruction in the
correct direction (moving down needs the WAR/WAW/RAW checks in `depends_on`; moving up likewise).
Do not cross control flow, DRC waits/invalidates (`pco_schedule.c` output), end markers, or
memory/tex instructions; run it before `pco_end` so the END/OLCHK pseudo ops are not crossed.
**Why:** recovers pairs that pre-RA placement created but RA/legalize destroyed, lets the pairing
decision see the final register numbers (the thing the current TODO complains about), and allows
backward placement, which post-RA `try_dual_issue` cannot do at all (it has no `find_sink`
equivalent; `find_sink` is only in the pre-RA pass). On `math`, 4 pairable p0 hosts remain
unpaired, some because the chosen candidate was dependency-blocked and no second candidate was
considered - a post-RA pass can choose among all candidates.
**Risk:** medium. Correctness relies on complete hardware-register dependency checks on a
non-SSA IR: keep it to pure ALU/bitwise ops and add the same asserts used by `try_dual_issue`.
It also interacts with RA decisions (a pair created post-RA cannot reduce temps; if placement
increases pressure it cannot - registers are already fixed, so this is a pure win vs pre-RA).
**Measure:** `PCO_DEBUG=no_dual_issue` A/B + group count; math/ilp/real FS; #pairs should go up
without group-count growth; CTS gate.

### 4. Run `pco_schedule` (WDF/IDF placement) after RA, per the `pco_ir.c:52` TODO
**Where:** `pco_ir.c:52-56` (TODO + call site), `pco_schedule.c:26-58`.
**What:** `pco_schedule` is not a list scheduler: it only inserts the wait/invalidate fences for
DRC (device result count) results at the *first use* (`pco_for_each_instr_src` "first source is
DRC"). It runs before RA, where the first use can still be a `vec` pseudo instruction, or a use
that RA/legalize later moves or dissolves; after RA (vecs are gone, legalizer MOVs inserted) the
true consumer is known. Move the pass to after `pco_post_ra_legalize` (before `pco_end` -
inserting a group after `pco_end`/END is wrong). Note `pco_schedule` also handles the ST32/IDF
form and needs the DRC refs preserved by RA (they are: DRC refs are not SSA and are ignored by
RA).
**Why:** a wait placed earlier than the true first use serialises for no reason; a wait placed
later than a moved use is a correctness bug, so this is also the more conservative placement. The
TODO explicitly asks for this. Round 203 measured *skipping* the pass (neutral), not moving it;
the `math` FS has only one WDF so it is not a meaningful test - this targets texture/DRC-heavy
shaders (glmark2 shadow/ideas/refract, cstp).
**Risk:** low (one-line move + validate). Watch the ST32/IDF special case for `drc` in stores.
**Measure:** texture-heavy db2 modes (`db2 texture`/`db2 fill` and a cstp-like trace), IR diff to
confirm fence position moves later, CTS.

### 5. Saturate/clamp folding (instruction selection; medium-low)
**Where:** `pco_opt.c:574-600` (opt pipeline) / `pco_nir_algebraic.py` (103 lines of pco algebraic
rules); existing sat support already in `pco_trans_nir.c:3967-3990` (fsat -> `.sat` modifier on
fadd/fmul/fmad).
**What:** `fmin(fmax(x, c0), c1)` (GLSL `clamp(x,0.0,1.0)`) is emitted as two min/max groups; each
min/max is a 4-slot group (`mbyp` p0 + `mbyp` p1 + `tst` p2 + `movc` p2, `pco_group_instrs.c`
min/max folding, `trans_min_max` `pco_trans_nir.c:3656`). When the intermediate has a single use
and the producer is fadd/fmul/fmad, fold the upper clamp into the `.sat` modifier of the producer
and drop one group. Add a pco-nir algebraic rule or an opt pass matching `fmin(fmax(alu, lo), hi)`
with lo/hi constants and lowering to the producer's sat (only when the alu result is single-use).
`fmin/fmax` itself is already optimal (one group, NaN handling in `pco_nir_alu.c:44-90`), so do
not try to re-encode min/max.
**Why:** saturate is extremely common in shading/bump/effect2d fragment shaders. Low risk but
unquantified without a count; measure clamp occurrences first via an IR dump of the actual
glmark2 shaders.
**Risk:** low-medium (NaN/signed-zero semantics must match the NIR fp_math_ctrl flags; follow the
existing `pco_nir_alu.c` min/max NaN-expansion pattern).
**Measure:** group count on bump/effect2d FS dumps.

### 6. `pow(x, small-int-constant)` -> repeated squaring (low)
**Where:** `pco_nir.c:66` sets `.lower_fpow = true`, so NIR lowers `pow` to
`fexp2(y*flog2(x))` before pco (`pco_trans_nir.c:3955-3961` maps exp2/log2 to fexp/flog). Add a
rule (pco algebraic, after the fpow lowering) matching `fexp2(fmul(flog2(x), K))` for small
integer/power-of-two K and replacing with a squaring chain.
**Why:** in the `math` FS `pow(x,16)` = flog + fmul + fexp = 3 groups, two of which are
single-unit, unpairable, high-latency instructions; 4 FMULs pair into ~2-3 group slots and avoid
the transcendental latency. But the fidelity requirement is highp and only constant exponents
benefit; glmark2 phong uses a uniform exponent, so this is for real-world shaders/future tests -
rank last.
**Risk:** low-medium (rounding differs; highp permits it; x<0 already undefined for GLSL pow).
**Measure:** `math` FS groups 26 -> 27 but slots; fps only if that shader matters.

### Rejected / already covered (avoid wasted work)
- **RA bank/index awareness for p1 encodability:** temps are all `PCO_REGBANK_TEMP` (bank value
  1 -> `util_last_bit` = 1 bit, `pco_map.h.py:84-89`), so sB/sC's 2-bank-bit limit never rejects a
  temp, and p1 index limits (8 bits = 256) exceed the 64-register temp file. The only refs that
  fail `p1_srcs_ok` are shared (literal/uniform) and indexed registers in p1 source 1/2, which
  `swap_commutative_srcs()` already handles for fadd/fmul. Don't spend time here; candidate 1/2
  already reduce the shared-register cases further.
- **Pairing window tuning:** round 200 measured window 4 (neutral) and 16 (-25% on math). Keep 8.
- **min/max encoding:** single group already (`try_fold_into_next` gives the 2->1 group fold,
  commit `0d20f6c1`); no cheaper main-ALU min/max exists on this ISA.
- **abs/neg/modifier folding:** working (dump shows `.abs`, `.neg`, sat, floor modifiers folded
  into p0/p1 slots).
- **sqrt:** `frsq`+`frcp` is required (no FSQRT main op in `pco_isa.py`); two single-unit ops.
- **F16SOP / FP16:** deprioritised by round 186 (this silicon's FP16 rate ~= FP32 rate).

## Offline compile / instruction-mix path (item 4 answer)
- pco has **no unit tests or standalone compile tool** in tree: `src/imagination/pco/meson.build`
  builds only `libpowervr_compiler` + generated headers + usclib; there is no `tests/` dir
  (`find src/imagination -name '*test*'` empty). `pco_print_shader()` / `pco_print_shader_stats()`
  are callable but the only in-tree callers are the Vulkan/Gallium drivers, which need a DRM
  device, so `PCO_DEBUG_PRINT=...` is **not usable offline** as-is.
- A CPU-only harness **is feasible** with the existing build tree:
  `build/src/imagination/pco/libpowervr_compiler.a` exports `pco_ctx_create`, `pco_trans_nir`,
  `pco_process_ir`, `pco_encode_ir`, `pco_print_shader`, `pco_shader_binary_data` (verified with
  `nm`); `build/src/compiler/libcompiler.a` (NIR), `build/src/util/libmesa_util.a`, generated
  headers under `build/src/imagination/pco/`, and the usclib blob
  (`build/src/imagination/pco/usclib/pco_usclib.{h,cpp}`) are all present. A ~200-line C harness
  would build NIR with `nir_builder`, fill a `pvr_device_info` for 36.56.104.183 (same table the
  driver uses), call the same sequence as `pvr_shader_compile_data` (preprocess -> lower ->
  postprocess -> trans -> process_ir -> encode) and dump IR/stats; no GPU needed. Useful for
  offline A/B of candidates 2/3/5/6 (group counts, pair counts, code size). Not built in this
  session; the saved dumps below were sufficient.
- Saved IR used here (pre-existing, no runs in this session):
  `/tmp/pco-math3.txt` (db2 `math` FS, current tree, Nov 1 02:41),
  `/tmp/ilp-ir.log` (db2 `ilp` FS, older build with per-iteration literals),
  `/home/radxa/open-perf/pco-{vkheavy,vkrender,cstpf,cstpi}.txt` (Vulkan driver,
  Oct 9-10 build; vkheavy is the pre-dual-issue 944-group heavy FS that commit `26ed8163`
  reduced to 786).

## Suggested order of work
1. Candidate 1 (driver plumbing, independent of pco, largest measured precedent, unblocks the
   other pco work).
2. Candidate 2 (small pco change, makes literal-heavy streams pair at all; validate group/pair
   counts on `math`, `ilp`, and a real glmark2 bump/effect2d FS dump).
3. Candidate 3 (bigger, keep it minimal: pure ALU within block, reuse existing helpers; A/B with
   `no_dual_issue`).
4. Candidate 4 (one-line pass move; validate on texture-heavy traces).
5/6. Candidates 5/6 only after a scene profile/count shows they matter.

Unknowns / NDA: register bank conflict rules and the exact phase-1 source-bank behaviour are not
in the public ISA reference; whether the vendor compiler's extra ~20% is pair density, bank
conflicts or scheduling cannot be proven without a vendor-compiled USC sample (vendor-usc-hunter
work). All "expected effect" numbers above are group/slot accounting, not measured wall time.

Measurement recipe used by the project: `PCO_DEBUG_PRINT=fs,stats` (not `PCO_DEBUG`) dumps IR +
`FS shader: <size> code size ... <n> inst group count`; `open-perf/shaderstat.c`
compiles a GLSL pair and draws one triangle; db2 modes `math`/`ilp`/`trans`/`alu` for fps; always
interleaved A/B, medians, restore vendor + desktop afterwards.
