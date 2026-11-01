# f16sop layout derivation (vendor mediump = packed-half math)

The vendor compiler lowers `mediump` GLSL arithmetic to **F16SOP** instructions
(the pco ISA tables have `# F16SOP # TODO`, so pco cannot emit them). This file
derives the encoding from the vendor's own output.

## Method

1. `f16sop_catalogue.py` walks every captured vendor USC blob and prints each
   f16sop instruction with its raw payload (see `f16sop-samples/catalogue.txt`).
2. Purpose-built controlled GLES2 FS samples (uniform-driven, nothing folds),
   one semantic change at a time, captured through the vendor driver with the
   existing pipeline: `shaders/<sample>/` + `cap_<sample>/`.
   - `t1_copy a`, `t2_add a+b`, `t3_sub a-b`, `t4_negb a+(-b)`, `t5_nega (-a)+b`,
     `t6_absa abs(a)+b`, `t7_absb a+abs(b)`, `t8_mul a*b`, `t9_mulnegb a*(-b)`,
     `t10_mulabsa abs(a)*b`, `t11_min min(a,b)`, `t12_max max(a,b)`,
     `t13_swz a*b.yxwz`, `t14_swz2 a*b.zwxy`, `t15_neg -a`, `t16_mad a*b+c`,
     `u1_add_swap` (declaration order swapped), `u2_add_c` (extra uniform),
     `u3_add_swz a.wzyx+b`.
   - Earlier round: `s1_mad`, `s2_mul` … `s16_mul4`.
3. Differential analysis: samples with identical operands isolate op/modifier
   fields; samples with identical op isolate argument/component fields.
4. Cross-check against the vendor's own instruction-descriptor table
   (`g_psInstDesc`) extracted from `/lib/libusc.so` —
   `psInstDesc-sop.txt`.

## The instruction

An f16sop is a `sngl`-family extended opcode: byte0 = `0x94`
(main_op = SNGL = 4, ext0 = 1, sngl_op = F16SOP = 4), followed by the SOP
payload (the SOP form embeds its arguments; it is not a plain 2-byte sngl).

Captured SOPEXPR payload for `a + b` (mediump vec4 uniforms, first half):

```
idx:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20
    94 99 99 af 00 c0 32 24 00 00 0e 81 44 81 00 88 85 08 00 40
```

## Derived fields

| bytes | meaning | evidence | confidence |
|---|---|---|---|
| 0 | `0x94` = main_op SNGL(4), ext0 1, sngl_op F16SOP(4) | all 82 captured f16sop instructions | **high** |
| 1-2 | SOP variant/format word: `99 99` = SOPEXPR (add/sub/min/max/abs/copy); `33 22` = multiply form; `92 93` = SOPMAD form | byte-identical for all operands within a form; differs between forms (t2/t8/t16) | **high** |
| 4 | operation selector: add/sub = `0x00`, min = `0x12`, max = `0x1b` | t2 vs t11 vs t12 (identical operands) | **high** |
| 8 bits 0,2 (`0x05`) | negate on argument 1 | `a-b` and `a+(-b)` are byte-identical (t3 == t4); `(-a)+b` canonicalises to the same form (commutative swap) | **high** |
| 9 bits 1,3 (`0x0a`) | abs modifier | `abs(a)+b` and `a+abs(b)` are byte-identical (t6 == t7; compiler canonicalises abs to one argument slot) | **high** |
| 11, 14 | argument 0 component/swizzle select | `a.wzyx + b` changes only these bytes (`81`→`82`, `00`→`03`) | **medium** |
| 16 | argument 1 component/swizzle select (multiply form) | `a*b` 0x85, `a*b.yxwz` 0x84, `a*b.zwxy` 0x87 | **medium** |
| 3, 5, 6, 7, 10 | not yet mapped (constant across the SOPEXPR samples, differ between forms) | — | low |
| 11-21 (rest) | inline argument register/format and dest encoding | see below | low |

Declaring the uniforms in the opposite order (`u1_add_swap`) produces
byte-identical code, and adding an unused uniform (`u2_add_c`) does too — the
argument bytes do not carry the declaration order, so operands are referenced
by fixed coefficient-bank slots (or the argument encoding was not exercised).

## Vendor instruction-descriptor evidence

`libusc.so` contains the USE-assembler instruction table `g_psInstDesc`
(32-byte entries, mnemonic at +0, six u32 fields after). SOP entries extracted
in `psInstDesc-sop.txt`:

```
f16sop        f16sop.ext   f16sop.mad   f16sop.u8   f16sop.u8mad
sopj sopj.ext sopj.u8       sopk sopk.ext sopk.u8
sopmov sopmov.ext sopmov.u8 sopmad0..3      sopu8mad0..3  sopu8madmov
```

The compiler's internal IR names (assertion strings in libusc) confirm the
semantics: `SOPADD`, `SOPSUB`, `SOPMAX`, `SOPMIN`, `ISOPEXPR`, `ISOPMAD`,
`ISOPPCKU8`, `asArgMod[].bNegate/bAbsolute/bClamp`, `eSopDestFormat`,
`bExtSop`, source file `compiler/usc/rogue/f16replace.c`. So the full op set is
add/sub/mul/min/max/mad/u8-pack/mov with per-argument negate/abs modifiers and
a dest format — which matches the fields derived above.

## Round-trip check

`f16sop_roundtrip.py` implements the derived fields and reproduces the raw
payload bytes of the controlled SOPEXPR samples:

```
t2_add   OK     t3_sub  OK     t4_negb OK    t11_min OK
t12_max  OK     t6_absa OK     t7_absb OK
7/7 round-trip checks passed
```

It applies the derived op selector / negate / abs bits to the captured `a+b`
operand template. It therefore validates the **field positions**, not a complete
instruction encoder — the inline argument/dest bytes are still taken from the
capture.

## What is still missing (and what evidence would close it)

The inline argument encoding (register bank/index, per-argument format,
destination) is not derived. To finish it we need one of:

1. **Register-offset differential**: force the same uniform to land in different
   coefficient slots (e.g. two used uniforms before `a`, or `gl_FragColor.rg =
   ...` to change the dest mask) and diff; the declaration-order experiment
   above did not move the operands, so this needs explicit slot forcing.
2. **Format differential**: same op with f16 vs u8 vs c10 operands
   (`texture2D` results, packed colour) to map the format fields.
3. **Vendor assembler tables**: `libusc.so`'s USE assembler encodes these
   instructions, but the encode tables are not exported (only the descriptor
   table above is reachable without reversing the stripped encoder code).
