# F16SOP / mediump packed-half implementation plan for pco

Date: 2026-11-01 (round-217, teammate pco-isa-analyst)
Tree: `/home/radxa/mesa/mesa-main` @ `6e9baf84`, clean, read-only analysis (no board access,
no GPU runs, nothing written outside `open-perf/`).
Basis: round-216 root cause (vendor bump FS shows pco is missing F16SOP), pre-existing
vendor captures in `/home/radxa/open-perf/vendor-usc/`, and the pco sources. `REPORT.md`
(codegen analysis) is unchanged; this is a new plan file.

Target numbers from round-216: bump FS 33 pco groups -> 17 (vendor), conv9 FS 58 -> 31, and
the 1.5-2x scene deficits on bump/shading/effect2d. Target output of this plan: pco emits
F16SOP for mediump math and the group counts/fps move to those numbers.

## 0. Decision summary

1. Add F16SOP to the pco ISA/op/map tables (3 hand-written source files; the rest is generated).
2. Represent NIR f16 values in pco as **32-bit "packed-half pair" refs**: an f16 value of N
   components maps to `ceil(N/2)` 32-bit channels, each channel holding two halves
   (W0[15:0], W0[31:16]). This is the same trick pco already uses for 64-bit
   (`pco_trans_nir.c:84-90`: one 64-bit value -> two 32-bit channels), and it means
   **RA, legalize, grouping, scheduling and the encoder need no changes** — they never see a
   16-bit ref. This is the single most important design choice; do not teach RA 16-bit
   channels.
3. Emit F16SOP from a new NIR pass that demotes mediump ALU chains to f16 (GLES2 mediump is
   currently fp32 end to end), plus a small `trans_alu_f16()` in `pco_trans_nir.c`.
4. Keep f16 **vector** ALU alive: pco scalarises every ALU op at `pco_nir.c:594`; that call
   must stop scalarising 16-bit ops, otherwise packing is impossible (scalar half values
   cannot be paired for free).
5. Gate behind an env knob during bring-up (`PVR_ENABLE_MEDIUMP_F16`), per shader, FS first.
6. Blocked on one deliverable from vendor-usc-hunter: the exact F16SOP instruction/operand
   bit layout (S0 below). Everything else is independent and can be built in parallel.

## 1. How half ops reach pco today (Q1)

### GLES2 mediump is fp32 today
- GLSL ES mediump is only metadata on `nir_variable::data.precision`. Mesa lowers mediump
  *variables* to 16-bit only if the screen advertises both `shader_caps[stage].fp16` and
  `.int16` (`src/compiler/glsl/gl_nir_linker.c:1360-1364`; fields in
  `src/gallium/include/pipe/p_defines.h:809-815`). `pvr_screen.c:441-484` sets neither, so
  mediump arrives at the driver as fp32 with only the precision flag preserved.
- Even with those caps set, `nir_lower_mediump_vars()` only changes variable storage and
  inserts f2f32/f2f16 at loads/stores (`src/compiler/nir/nir_lower_mediump.c:208-345`); it
  does **not** produce fp16 ALU. So the mediump->fp16 ALU demotion has to be written for
  this driver regardless of which switch turns it on.
- pco therefore never sees f16 ALU from GLES2 today. The `.lower_flrp16 = true` comment in
  `pco_nir.c:57-61` ("mediump in GLES can be fp16 ... glmark2 cel") is defensive: no fp16 ALU
  is emitted by the current pipeline.

### Vulkan FP16
- `shaderFloat16` is off by default (`pvr_physical_device.c:354-367`: `PVRSRV_FP16` env
  force-enables, and the comment records 27 failure with 20 failing). If forced, SPIR-V
  explicit 16-bit types produce NIR fp16 values.
- pco's existing 16-bit support is *I/O only*: `pco_nir_16bit_io.c` widens 16-bit shader I/O
  to 32 bits (called at `pco_nir.c:1265`); conversions f2f16/f2f32 use PCK/UNPCK
  (`trans_conv`, `pco_trans_nir.c:3753-3850`) storing halves in 32-bit registers.
- There is no fp16 ALU: `trans_alu()` (`pco_trans_nir.c:3893`) always emits the 32-bit fadd/
  fmul/fmad builders, and RA asserts only 32-bit dests (`pco_ra.c:500-505`). That is why the
  forced-Vulkan-FP16 path is broken; the same work fixes it later.
- pco ISA has the opcode slot but nothing else: `sngl_op = f16sop (0b0100)` exists in
  `pco_isa.py:929-946`; the bit struct is the literal `# F16SOP` / `# TODO` at
  `pco_isa.py:1383-1384`; there is no `O_F16SOP` in `pco_ops.py` and no `encode_map` in
  `pco_map.py`.
- pco value model: refs are `(class, index, bits, chans)`; `pco_ref_nir_def()`
  (`pco_trans_nir.c:82-92`) already special-cases 64-bit (bits 64 -> 32, chans*2) but leaves
  16-bit untouched, which RA cannot handle.
- pco scalarises ALU in the `pco_nir_opt()` fixpoint (`pco_nir.c:594`,
  `nir_lower_alu_to_scalar(nir, NULL, NULL)`), so NIR vectors never reach `trans_alu`.

## 2. Value representation (design)

- A NIR f16 vector of N components (N = 1..4) stays 16-bit in NIR; it is legal NIR and
  survives passes as `def->bit_size == 16`.
- pco internal mapping (new): `bits = 32`, `chans = DIV_ROUND_UP(N, 2)`. Channel k holds
  components 2k+1 / 2k as the two halves of the dword. Half order (which lane is
  W0[15:0]) must be verified against the vendor capture in S0; the public doc quotes
  W0[31:16]/[15:0] but not the operand order.
- Implement in `pco_ref_nir_def()` behind a `trans_ctx` flag (`f16_emit`), so explicit-fp16
  paths that are not supported yet keep the old behaviour and are caught by the RA assert
  rather than silently miscompiled.
- Everything downstream works unchanged: RA (contig channel classes), constant-immediate
  legalisation (`ROGUE_MAX_REG_OFFSET` offset math), scheduling, grouping and the encoder
  all operate on 32-bit refs.
- Component munging: pco has VEC/COMP pseudo ops for multi-channel SSA (helper at
  `pco_trans_nir.c:175-195`, used from `trans_instr`). For packed halves the first version
  should accept only whole-vector f16 ALU plus f2f16/f2f32 boundaries; any NIR `vec`,
  extract or swizzle on f16 values that the helper cannot express must make the shader fall
  back to fp32 (see gating). `SOPMOV` (docs) can express a half move/pack later.

## 3. ISA and lookup-table work (Q2)

### Ground truth already available
Vendor bump FS (`/home/radxa/open-perf/vendor-usc/captures/bump/extracted/usc_fs.bin`,
decoded by `vendor-usc/disasm.py`, listing in `.../disasm/fs.txt`): 17 groups / 264 B, 9 of
them F16SOP. Decoding the raw bytes (my analysis, this session):
- instruction word0 is `0x94` in all 9 groups: bits 7:5 = 100 (`main_op = sngl`), bit 4 = 1
  (`ext0`), bits 3:0 = 0100 (`sngl_op = f16sop`). This matches pco's `I_SNGL_EXT` layout
  exactly (`pco_isa.py:1369-1381`).
- word1 bits 1:0 match `s0neg`/`s0abs` at the same positions as `I_SNGL_EXT`
  (`s0neg_sngl`, `s0abs_sngl` in `pco_isa.py:1099-1300`): byte1 values are
  0x32/0xab/0x9a/0x99/0x92/0x92/0x99/0x99/0x99, which is exactly the neg/abs combination
  disasm printed for each group.
- word1 bits 7:2 carry F16SOP-specific fields: only four distinct patterns in the whole
  shader (byte1>>2 = 0x0c, 0x2a, 0x26, 0x24). Group instructions are 2 bytes (header
  decode-assist da=2 with oporg=p0), so word2 is unused in this form.
- Sources/dests are packed into 16-24-byte groups (e.g. g5's stream is
  `49 04 07 40 22 22 00 00 af 99 99 b0 a4 62 00 40 41 83 24 ff`), consistent with 3 source
  operands and/or packed 32-bit half-pair literals in the operand stream. The operand
  streams are not decoded yet.

Open question S0 must settle: public docs list jOp in {add,sub,min,max,rsub,mad} (6 values)
and kOp in {add,sub,min,max,rsub} (5 values) plus rfmt and ONEMINUS modifiers — that is more
than the 6 free bits in word1[7:2], unless (a) some values are encoded by the source stream/
immediate selection, (b) there is a longer (3-byte, word2) form for MAD/U8/U8MAD that bump
FS does not use, or (c) several modifier bits live in the source-encoding extension bits.
Do not guess this; it is the S0 acceptance criterion.

### Files to change
1. `pco_isa.py` (hand-written ISA tables; everything else is generated):
   - after `F_SNGL_OP` (`:929-946`): new field enums `F_F16_JOP`, `F_F16_KOP` and (if
     separate bits) `F_F16_RFMT`, `F_F16_ONEMINUS` (maybe `F_F16_MAD`/`F_F16_U8`); or a
     single F16 OP enum if S0 finds a combined encoding.
   - `I_MAIN` bit_set (`:1099-1300`): add named `pieces` at the F16SOP field bit positions
     (word1 [7:2], word2 if used) and matching `fields` entries. Alternative encodings
     already overlap bits (ext0 vs sat_fam vs s_i816), so new names on the same physical
     bits are the established pattern; do not rename existing pieces.
   - replace `# F16SOP / # TODO` (`:1383-1384`) with `I_F16SOP` / `I_F16SOP_EXT` (and
     `I_SOPMOV` if SOPMOV is a separate sngl encoding). Model them on `I_SNGL` /
     `I_SNGL_EXT` (`:1360-1381`); `I_F16SOP_EXT` carries `main_op=sngl, ext0=1,
     sngl_op=f16sop` plus s0neg/s0abs plus the new jOp/kOp/rfmt/oneminus fields.
   - `pco_isa.h.py` generates enums, per-field encode functions, per-variant byte sizes and
     the struct encode functions automatically (`pco_isa.h.py:52-114`) — no generator edits.
2. `pco_ops.py` (op semantics + builder API source):
   - new op mods near `OM_F16` (`:223`; existing `OM_F16` is the DITR/SMP 16-bit
     interpolation/sample flag and is unrelated — leave it alone): `OM_F16_JOP`,
     `OM_F16_KOP`, `OM_F16_RFMT` (bool), `OM_F16_ONEMINUS` (bool), optionally `OM_F16_MAD`.
   - new op next to `O_FRCP`/`O_FRED` (`:367-390`), following the `hw_op(name, mods,
     ndests, nsrcs, imm, per-source-mod-sets)` signature (fadd at `:367`, multi-dest example
     FRED at `:376`): `O_F16SOP = hw_op('f16sop', OM_ALU + [OM_F16_JOP, OM_F16_KOP,
     OM_F16_RFMT, ...], ndest, nsrc, [], [[RM_ABS, RM_NEG], ...])`. `ndest/nsrc` from S0;
     expect 1 dest, 2-3 sources (MAD form adds a third) — the vendor streams suggest up to 3.
   - optional internal `O_SOPMOV` for the documented SOPMOV form.
3. `pco_map.py` (op -> bit-struct variant selection + hardware source/dest slot mapping):
   - add `encode_map(O_F16SOP, encodings=[(I_F16SOP_EXT, [...mod->field and
     field-value mappings..., ('s0neg',(RM_NEG,SRC(0))), ('s0abs',(RM_ABS,SRC(0)))]),
     (I_F16SOP, [...])], op_ref_maps=[('0', ['w0'], ['s0','s1' (, 's2')])])` after the
     O_FRCP block (`:929-1003`); O_FRED (`:1004`) is the multi-dest reference, O_PCK
     (`:1118`) the sngl-op reference. This single table drives `pco_map_igrp`,
     `pco_igrp_*_variant`, the generated `pco_*_variant()` functions, and the
     legalizer's source/dest hardware-slot mapping; no new C code in `pco_legalize.c`
     unless F16SOP needs a source that cannot be placed in S0/S1 (existing
     `insert_mov_ref`, `pco_legalize.c:81-120`, then handles it automatically).
   - `group_maps` and the `op_ref_maps` machinery are generated from the op declaration
     (`pco_map.py:855-860`); verify every op still has a map (the generator/index likely
     asserts).
   - packed half immediates: the reference driver's groups carry long operand streams. pco
     main-ALU source encodings are register-only today (`I_SRC` variants,
     `pco_isa.py:462+`; constants are materialised as `bbyp0bm_imm32` bitwise groups,
     `pco_ops.py:460-461`). First version: materialise packed constants with one
     `bbyp0bm_imm32` group per pair (already 2x better than two fp32 constants); extending
     `I_SRC` with immediate forms is an S5 optimisation, only if S0 shows F16SOP operands
     can be immediates.
4. Generated automatically from the two files: `pco_ops.h` (op enum/op_info/builder
   prototypes), `pco_builder_ops.h` (`pco_f16sop(&b, ...)`), `pco_common.h` (field enums),
   `pco_isa.h` (encode tables), `pco_info.c`, print names. `pco_binary.c` is generic
   (`pco_encode_igrp`, `pco_binary.c:60-105`). `pco_print.c` prints ops/mods generically via
   op_info — verify with `PCO_DEBUG_PRINT=fs`.
5. `pco_validate.c`: generic op/ref checks (`:395-405`) need no change if operands are
   32-bit refs; add an explicit assert in the translator that an F16SOP dest/src ref has
   `bits == 32` (packed pair, not a raw 16-bit ref).
6. `pco_group_instrs.c`: add F16SOP to `is_plain_alu_op()` (`:210-234`) so the dual-issue
   window scan can cross it (it is a pure default-exec ALU op), but **not** to `is_p1_op()`
   (`:175-187`) or `is_p0_pairable()` (`:189-207`): vendor F16SOP groups are single-phase
   (oporg=p0) and the p1 op set is hardware-limited to fadd/fmul/fmad/mbyp. `try_fold_into_next`
   (`:455-524`) needs no change.
7. `pco_ra.c`: no change by design; keep the `only_32bit` assert (`:500-505`) as the tripwire
   that no un-packed 16-bit ref ever reaches RA.

## 4. Lowering and emission (Q3)

### 4a. New NIR pass: mediump ALU demotion (`pco_nir_lower_mediump_alu`)
New pass (new file `pco_nir_mediump.c`, or extend `pco_nir_alu.c`), run in the driver
**after** mediump variables are 16-bit and **before** pco's optimisation/scalarisation:

Algorithm (fixpoint, `nir_shader_alu_pass` + builder, keep vectors):
1. Anchor: f16 variable loads/stores appear as `f2f32(load_deref f16)` and
   `store_deref(f2f16(x))` after `nir_lower_mediump_vars`.
2. Starting from `f2f16(x)` sinks, walk the producer chain backwards: convert an f32 ALU op
   to the f16 equivalent if (a) every source resolves to a mediump value (f16 const, f16
   load, or already-converted op), and (b) the op is in the supported set
   {fadd, fsub, fmul, ffma, fneg, fabs, fmin, fmax} (extend as S0/S1 confirm encodings).
   Drop redundant f2f32/f2f16 pairs as conversions move to the boundary.
3. Bail out per shader if any f16 value would need an unsupported op/pattern — keep the
   whole shader fp32 (correctness first; mixed is a later optimisation). This pass must
   also not demote values used by highp consumers (the GLSL ES precision rule "result is
   at most the highest operand precision" makes all-mediump chains fair game).
4. It must run before `nir_lower_alu_to_scalar` so the vector shape survives
   (`pco_nir.c:594`). Recommended call sites: driver `pvr_shader_compile_data` after
   `nir_lower_mediump_vars`/before `pco_preprocess_nir`; if optimiser interactions bite,
   move both to the top of `pco_preprocess_nir`. Do not run it at the end of the pco
   pipeline: by then pco has already scalarised every ALU op (`pco_nir.c:594`).

Mapping table NIR f16 -> F16SOP (to be finalised by S0):
- fadd/fsub: jOp/kOp add/sub or rsub (sub = add with s0neg on the subtrahend; vendor uses
  s0neg/s0abs in 9/9 groups, so source modifiers are the expected mechanism).
- fmul: docs' jOp/kOp sets do not list a plain multiply; likely a MAD form with zero addend,
  or an ONEMINUS/rfmt variant — S0 must say which encoding the 0x24/0x26 words are.
- ffma: F16SOP.MAD form (jOp=kOp=mad? docs list mad only for jOp; S0 resolves).
- fmin/fmax: min/max jOp/kOp (docs); NaN/signed-zero semantics need NIR `fp_math_ctrl`
  modelling like the f32 path (`pco_nir_alu.c:44-90`).
- fneg/fabs: source modifiers s0neg/s0abs (confirmed in use by the vendor).
- `1 - x`: ONEMINUS (docs).
- f2f16/f2f32 at the boundary stay PCK/UNPCK (`trans_conv`, `pco_trans_nir.c:3753-3850`);
  add two-lane forms so two f32 components pack with one group where possible.

### 4b. pco translator
- `pco_ref_nir_def()` (`pco_trans_nir.c:82-92`): when `tctx->f16_emit` and
  `def->bit_size == 16`, map to `bits = 32, chans = DIV_ROUND_UP(def->num_components, 2)`
  (same shape as the existing 64-bit special case).
- `trans_alu()` (`pco_trans_nir.c:3893`): add an early `if (alu->def.bit_size == 16)
  return trans_alu_f16(tctx, alu);`. `trans_alu_f16()` loops channels, emits one F16SOP per
  packed pair with jOp/kOp/mods from the NIR op, and fills odd lanes with undef (junk lane
  is never read; keep min/max/NaN-safe).
- VEC/COMP handling for packed halves: `pco_vec`/`pco_comp` helpers
  (`pco_trans_nir.c:175-195`) produce pco pseudo ops for multi-channel SSA. First version:
  only support f16 values flowing ALU->ALU or ALU->conversion; if the demotion pass leaves
  an f16 `vec`/extract/swizzle, bail the shader to fp32. Later: SOPMOV/ONEMINUS-based
  pack/unpack.

### 4c. Gating
- Bring-up: env `PVR_ENABLE_MEDIUMP_F16=1` read in `pvr_shader.c` (same style as
  `PVR_NATIVE_DEBUG` flags), per-shader decision stored in local driver data; FS first, VS
  later.
- Enablement source of mediump: call
  `nir_lower_mediump_vars(nir, nir_var_function_temp | nir_var_shader_temp)` directly in
  `pvr_shader_compile_data` (before `pco_preprocess_nir`), with the same modes the GL linker
  would use (`gl_nir_linker.c:1364`). This avoids advertising
  `PIPE_SHADER_CAP` fp16/int16 in `pvr_screen.c:441-484` until the path is validated; a
  later switch to the caps route is a 2-line change (fields in
  `pipe_shader_caps`, `pipe/p_defines.h:809-815`).
- Device gating: base F16SOP is a main-ALU instruction and the vendor runs it on this
  BVNC; `has_usc_f16sop_u8` (`pvr_device_info.h:305`) is the U8 *packing* variant used by
  PBE/format paths (`pvr_csb.h:690-710`) and by F16SOP.U8/U8MAD forms — gate only those
  variants on it, not base F16SOP. No known USC config bit for half ALU; S1's numeric test
  settles it.

## 5. Staging and validation (Q4)

### S0 (blocker, vendor-usc-hunter; parallel with S1)
Deliverable: exact F16SOP encoding from the existing capture tooling
(`open-perf/vendor-usc/`, `disasm.py`, `capture.sh`, `run_scene.sh`):
1. Instruction length (2 vs 3 bytes) and field table for word1[7:2] (+word2 if used), with
   jOp/kOp/rfmt/ONEMINUS widths, from the 9 bump-FS words plus differential captures.
2. Semantics table: what operation each observed encoding is (map the four distinct bump
   words to the known Blinn-Phong GLSL: normalize/rsq, dot/mad chains, pow, saturate).
3. Operand-stream decode: extend `disasm.py` with I_SRC/I_DST decode from `pco_isa.py`
   tables (encode direction exists in `pco_map.py`/generated `pco_map.h`) and round-trip
   check: decode a vendor group, re-encode with pco, require byte equality.
4. Minimal differential GLSL->vendor pairs: half add, half mul, half fma, half min/max,
   1-x, and rfmt=f32 form, to enumerate encodings; captured from `run_scene.sh` with
   `glmsrc.gdb`.
Acceptance: field table + the bump FS words decode to a sequence that re-encodes
byte-identically.

### S1 pco ISA/op plumbing + round-trip (no NIR pipeline yet)
Implement section 3. Test without the board if possible: build a CPU-only compile harness
(see `REPORT.md` "Offline compile" section: `build/src/imagination/pco/libpowervr_compiler.a`
exports `pco_trans_nir/pco_process_ir/pco_encode_ir`; link `libcompiler.a`,
`libmesa_util.a`, generated headers and the usclib blob) that builds NIR with `nir_builder`
and dumps IR/binary. Acceptance:
- compiler emits word0=0x94 for each F16SOP op and the modifier bits match the configured
  jOp/kOp/mods;
- decode vendor group -> re-encode -> byte-identical (S0 tool);
- validator/print/RA assertions stay quiet (`PCO_DEBUG_PRINT=fs`, `nir_validate`).
- Numeric test needs hardware: hand-built shader (pattern of
  `pvr_draw.c:1065 nir_builder_init_simple_shader`) that packs two f16 halves, runs F16SOP,
  unpacks both lanes to f32 and writes them; compare expected f16 results. This is the
  hardware-enable smoke test.

### S2 translator + packed representation
`pco_ref_nir_def` packing, `trans_alu_f16`, scalariser filter (`pco_nir.c:594`), fallback
pass. Acceptance (offline harness): hand-built NIR with v2f16 fadd/fmul/ffma produces the
expected F16SOP groups and no other change; group counts in IR dump; NIR validation on.

### S3 mediump demotion + driver wiring
New pass + `nir_lower_mediump_vars` call in `pvr_shader.c`, env-gated. Compile the captured
glmark2 shaders (`vendor-usc/shaders/bump/src_00{1,2}.glsl`,
`shaders/effect2d-conv9/...`) through our driver; IR-dump group counts; targets:
bump FS **17** (from 33; vendor 17/264B), conv9 FS **31** (from 58). Then fps:
glmark2 bump/shading (current native ~413/961 vs vendor 672/1375), db2 math/ilp must not
regress, 17-group gate green.

### S4 correctness and precision
- Rendered output will differ from today's fp32 output by fp16 rounding — that is expected
  and matches the vendor (which also uses fp16). Any gate that compares exact pixel bytes
  needs a tolerance or vendor reference frames; decide before flipping the default on.
- Test NaN/Inf/signed-zero behaviour of min/max/mad against NIR `fp_math_ctrl`; GLES2
  mediump tolerance is lenient but CTS has precision tests.
- Run with the feature off to prove byte-identical shaders (parity baseline).

### S5 follow-ons (only after S3/S4)
packed f16 varyings (VS out/FS in; `pco_data.fs.f16_smooth/f16_flat` already exist,
`OM_F16` already on DITR/SMP in `pco_ops.py`), dot-product/`mad` mapping from NIR fdot,
packed immediate sources, explicit-fp16 Vulkan (`shaderFloat16` path), possible p1 pairing
of F16SOP if hardware allows.

## 6. Exact insertion points

| File | Line(s) | Change |
|---|---|---|
| `pco_isa.py` | 929-946 | new f16 jOp/kOp/rfmt/oneminus field enums after `F_SNGL_OP` |
| `pco_isa.py` | 1099-1300 | `I_MAIN` bit_set: add pieces/fields at F16SOP bit positions (word1[7:2], word2 if used) |
| `pco_isa.py` | 1360-1381 | model: `I_SNGL` / `I_SNGL_EXT` |
| `pco_isa.py` | 1383-1384 | replace `# F16SOP`/`# TODO` with `I_F16SOP` / `I_F16SOP_EXT` (+`I_SOPMOV`) |
| `pco_isa.py` | 462+ | `I_SRC` variants — only if S0 shows immediate operands (S5) |
| `pco_ops.py` | 223-224 | `OM_F16` exists (DITR/SMP); add `OM_F16_JOP/KOP/RFMT/ONEMINUS` nearby |
| `pco_ops.py` | 367-390 | add `O_F16SOP` after `O_FRCP`/`O_FRED` |
| `pco_map.py` | 929-1003 | add `encode_map(O_F16SOP, ...)` after the `O_FRCP` block |
| `pco_map.py` | 855-860 | group_maps built from op decls — verify new op included (generator asserts) |
| `pco_trans_nir.c` | 82-92 | `pco_ref_nir_def`: f16 -> packed 32-bit channels under `tctx->f16_emit` |
| `pco_trans_nir.c` | 3893 | `trans_alu`: dispatch `bit_size == 16` to new `trans_alu_f16()` |
| `pco_trans_nir.c` | 3931/3939/3943 | fadd/fmul/ffma cases = shape of f16 mapping |
| `pco_trans_nir.c` | 3753-3850 | `trans_conv` f2f16/f2f32: add two-lane pack/unpack forms |
| `pco_trans_nir.c` | 175-195 | `pco_vec`/`pco_comp` helpers for multi-channel SSA |
| `pco_nir.c` | 594 | `nir_lower_alu_to_scalar`: filter must keep 16-bit ALU vectors |
| `pco_nir.c` | 560-600 | `pco_nir_opt` fixpoint (where the scalariser lives) |
| `pco_nir.c` | 1265 | `pco_nir_lower_16bit_io` call (context; opposite direction for S5 varyings) |
| `pco_nir_alu.c` | 44-90 | NaN/signed-zero lowering pattern to follow for f16 min/max |
| `pvr_shader.c` | ~672-800 | `pvr_shader_compile_data`: call `nir_lower_mediump_vars` + new demotion pass before `pco_preprocess_nir`; env gate |
| `pvr_shader.c` | 291-400 | `pvr_shader_create_with`: store per-shader demotion decision |
| `pvr_screen.c` | 441-484 | only if switching to caps route: `fp16 = int16 = true` |
| `pco_group_instrs.c` | 210-234 | add F16SOP to `is_plain_alu_op` (crossable), not `is_p1_op`/`is_p0_pairable` |
| `pco_ra.c` | 500-505 | keep the 32-bit-only assert as the tripwire |
| `pco_validate.c` | 395-405 | generic checks; add bits==32 assertion for F16SOP refs |
| `vendor-usc/disasm.py` | operand decode | S0: I_SRC/I_DST decode + encode round-trip |
| `vendor-usc/captures/bump/disasm/fs.txt` | — | ground truth (9 F16SOP words) |

Generated (no manual edit): `pco_isa.h`, `pco_ops.h`, `pco_builder_ops.h`, `pco_common.h`
field enums, `pco_info.c`, `pco_map.h`.

## 7. Risks

1. **Encoding/semantics unknown** (S0 blocker). Bit-level layout is NDA-only; the public
   doc gives semantics only. Never guess: accept S0 only with byte-identical re-encode of
   captured vendor groups.
2. **Hardware enable**: expected none — F16SOP is a main-ALU instruction and the vendor emits
   it on this BVNC; `has_usc_f16sop_u8` is the U8 packing feature (PBE/format paths,
   `pvr_csb.h:690-710`), not an ALU enable. S1's on-board numeric test is the check; if it
   fails, look for a USC config/state bit (none exists in pco today).
3. **Precision/validation semantics**: mediump->fp16 changes pixels; golden-image gates that
   compare exact bytes will fail. Decide tolerance or vendor-reference policy before
   default-on. f16 min/max/mad NaN/signed-zero semantics must follow `fp_math_ctrl`
   (`pco_nir_alu.c:44-90` pattern).
4. **Mixed precision cost**: f32<->f16 conversions (PCK/UNPCK groups) can eat the win in
   shaders that mix highp uniforms/builtins with mediump math. Vendor bump FS
   (9 F16SOP + rsq/log/exp + conversions = 17 groups) shows the target is achievable;
   add a per-shader profitability check later.
5. **Scalarisation/vector width**: `pco_nir.c:594` currently destroys vectors; the filter
   change is mandatory. Other vector passes (`nir_opt_shrink_vectors` in the same loop) may
   shrink 2-lane f16 ops — validate with `NIR_VALIDATE` during bring-up; odd component
   counts need junk-lane padding that cannot fault.
6. **Dual issue**: F16SOP groups are single-phase (vendor oporg=p0) and cannot be a p1
   partner under pco's current rules; the win comes from packing two ops per instruction,
   not from pairing. Expect no dual-issue improvement; do not add F16SOP to the p1 sets
   without hardware evidence.
7. **RA / legalize**: untouched by the packed representation (the point of the design). The
   `only_32bit` assert (`pco_ra.c:500-505`) must stay; large-offset/indexed-register
   legalisation (`pco_legalize.c:240-280`) sees normal 32-bit refs.
8. **In-flight teammate changes**: `pco_group_instrs.c` and `pco_ir.c` are being modified
   for the dual-issue/scheduling candidates; the F16SOP work lives in
   `pco_isa.py`/`pco_ops.py`/`pco_map.py`/`pco_trans_nir.c`/`pco_nir.c`/`pvr_shader.c`
   (disjoint), with one 1-line addition to `pco_group_instrs.c` that can conflict — coordinate.
9. **f16 varyings are a separate stage**: without VS-out/FS-in packed halves, bump VS will
   not shrink (vendor bump VS 19 groups) and interpolation stays f32; `pco_nir_16bit_io.c`
   currently widens 16-bit I/O (`pco_nir.c:1265`), so S5 is the place to reverse that for
   mediump varyings using `pco_data.fs.f16_smooth/f16_flat` and `OM_F16` on DITR/SMP.
10. **Vulkan FP16 side effect**: the same representation fixes the forced
    `PVRSRV_FP16` path's miscompiles, but do not enable `shaderFloat16` in the same change;
    validate separately.

## 8. Why this should hit the target (evidence)

- Vendor bump FS: 17 groups / 264 B = 9 F16SOP + 1 control + 6 f32 (fmul x2, fmad x2, rsq,
  log, exp) + conversions; pco today: 33 groups of fp32. Packed half math halves the two-op
  vectors (diffuse/ambient/specular terms are vec4 dot/mad chains) where the bulk of bump's
  ALU is; the transcendentals stay f32 in both compilers.
- conv9 FS: vendor 31 groups vs pco 58 — same shape (9-tap convolution is 9 mediump dot
  products).
- The vendor GLSL is fully mediump (`shaders/bump/src_002.glsl` starts with
  `precision mediump float;`), so a working mediump demotion covers the whole shader, not a
  fraction; this is why the group-count win is large.
- Round-145's lp-fmad experiment (+2%) and round-186's vkpeak result were measuring the
  f32 path / the low-precision modifier, not packed half execution; F16SOP is two ops per
  instruction, which is the 1.9x group gap.
