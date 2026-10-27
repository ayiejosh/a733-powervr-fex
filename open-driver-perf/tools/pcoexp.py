import re
R = "/home/radxa/mesa/mesa-main/src/imagination/pco/"


def rd(p): return open(R + p).read()
def wr(p, s): open(R + p, "w").write(s)


def sub(s, pat, new, flags=re.S):
    m = re.findall(pat, s, flags)
    assert len(m) == 1, (pat[:60], len(m))
    return re.sub(pat, lambda m: new.replace(chr(92) + '1', m.group(0)), s, flags=flags)


# ---- pco_legalize.c: PCO_DITR_MODE and CFG-aware pre-fence ----
s = rd("pco_legalize.c")
if "#include <stdlib.h>" not in s:
    s = s.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdlib.h>\n", 1)
helpers = '''/* Experiment switches (PCO_DITR_MODE): -1 = default; 0 = no fence and no wdf after a ditr (timing upper bound,
 * unsafe); 2 = ditr with sched=wdf and neither fence nor separate wdf; 3 = no fences, separate wdf kept.
 */
int pco_ditr_mode(void);
int pco_ditr_mode(void)
{
   static int mode = -2;
   if (mode == -2) {
      const char *e = getenv("PCO_DITR_MODE");
      mode = e ? atoi(e) : -1;
   }
   return mode;
}

/* Whether a and b sit in the then and else bodies of the same if (with no loop around it): they never both execute
 * on one path, so a register overlap between them is not a hazard.
 */
static bool instrs_exclusive(pco_instr *a, pco_instr *b)
{
   static int off = -1;
   if (off < 0)
      off = getenv("PCO_NO_PREFENCE_FIX") ? 1 : 0;
   if (off)
      return false;
   for (pco_cf_node *na = &a->parent_block->cf_node; na->parent; na = na->parent) {
      if (na->parent->type != PCO_CF_NODE_TYPE_IF || na->flag != PCO_CF_NODE_FLAG_IF_THEN)
         continue;
      for (pco_cf_node *nb = &b->parent_block->cf_node; nb->parent; nb = nb->parent) {
         if (nb->parent != na->parent || nb->flag != PCO_CF_NODE_FLAG_IF_ELSE)
            continue;
         for (pco_cf_node *up = na->parent->parent; up; up = up->parent)
            if (up->type == PCO_CF_NODE_TYPE_LOOP)
               return false;
         return true;
      }
   }
   return false;
}

'''
s = sub(s, r"(static bool ditr_needs_pre_fence\(pco_instr \*instr\)\n\{)", helpers + "static bool ditr_needs_pre_fence(pco_instr *instr)\n{")
s = sub(s, r"(      if \(prev->op == PCO_OP_FENCE\)\n         break;\n)", "      if (prev->op == PCO_OP_FENCE)\n         break;\n      if (instrs_exclusive(prev, instr))\n         continue;\n")
s = sub(s, r"(static bool try_legalize_ditr_fence\(pco_instr \*instr\)\n\{\n   if \(instr->op != PCO_OP_DITR && instr->op != PCO_OP_DITRP\)\n      return false;\n)",
        r"\1" + '''   int mode = pco_ditr_mode();
   if (mode >= 0) {
      if (mode == 2)
         pco_instr_set_sched(instr, PCO_SCHED_WDF);
      return false; /* no fences in the experiment modes */
   }
''')
wr("pco_legalize.c", s)

# ---- pco_schedule.c: no separate wdf after a ditr in modes 0 and 2 ----
s = rd("pco_schedule.c")
if "#include <stdlib.h>" not in s:
    s = s.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdlib.h>\n", 1)
s = sub(s, r"(         if \(instr->op == PCO_OP_WDF \|\| instr->op == PCO_OP_IDF\)\n            continue;\n)",
        r"\1" + '''         if ((instr->op == PCO_OP_DITR || instr->op == PCO_OP_DITRP) &&
             (pco_ditr_mode() == 0 || pco_ditr_mode() == 2))
            continue;
''')
# declaration (definition lives in pco_legalize.c)
s = s.replace('#include "pco_internal.h"\n', '#include "pco_internal.h"\n\nint pco_ditr_mode(void);\n', 1)
wr("pco_schedule.c", s)

# ---- pco_opt.c: shrink vecs with collated sources ----
s = rd("pco_opt.c")
if "#include <stdlib.h>" not in s:
    s = s.replace("#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdlib.h>\n", 1)
s = sub(s, r"         /\* Can't shrink if has collated elems\. \*/\n         if \(!only_uncollated_elems\(vec\)\)\n            continue;\n",
        '''         /* A vec with collated elems is shrunk by whole sources, see below. */
         bool collated = !only_uncollated_elems(vec);
         if (collated && getenv("PCO_NO_SHRINK_FIX"))
            continue;
''')
s = sub(s, r"         /\* Whole vec used, skip\. \*/\n         if \(chans_used == chans\)\n            continue;\n\n?         /\* Update the vec\. \*/\n         vec->num_srcs = chans_used;\n         \*pdest = pco_ref_chans\(\*pdest, chans_used\);\n\n?         progress = true;\n",
        '''         /* Whole vec used, skip. */
         if (chans_used == chans)
            continue;

         if (collated) {
            /* Drop the trailing sources that start beyond the last used channel. */
            unsigned keep_srcs = 0;
            unsigned keep_chans = 0;
            while (keep_srcs < vec->num_srcs && keep_chans < chans_used)
               keep_chans += pco_ref_get_chans(vec->src[keep_srcs++]);
            if (keep_chans >= chans)
               continue;
            vec->num_srcs = keep_srcs;
            *pdest = pco_ref_chans(*pdest, keep_chans);
            progress = true;
            continue;
         }

         /* Update the vec. */
         vec->num_srcs = chans_used;
         *pdest = pco_ref_chans(*pdest, chans_used);
         progress = true;
''')
wr("pco_opt.c", s)

# ---- pco_nir_alu.c: NaN-preserving min/max with a constant operand ----
s = rd("pco_nir_alu.c")
for inc in ("#include <stdlib.h>", "#include <math.h>"):
    if inc not in s:
        s = s.replace('#include "pco_internal.h"\n', '#include "pco_internal.h"\n' + inc + "\n", 1)
s = sub(s, r"(      if \(!nir_alu_instr_is_nan_preserve\(alu\)\)\n         break;\n)",
        r"\1" + '''      /* With a constant, non-NaN operand second, the hardware compare already gives the NaN-preserving result: when
       * the other operand is NaN the compare is false and the constant is selected.
       */
      if (!getenv("PCO_NO_NMAX_FIX")) {
         for (unsigned i = 0; i < 2; i++) {
            nir_scalar c = nir_scalar_resolved(alu->src[i].src.ssa, alu->src[i].swizzle[0]);
            if (!nir_scalar_is_const(c) || isnan(nir_scalar_as_float(c)))
               continue;
            if (i == 0) {
               nir_alu_src tmp = alu->src[0];
               alu->src[0] = alu->src[1];
               alu->src[1] = tmp;
            }
            alu->fp_math_ctrl &= ~nir_fp_preserve_nan;
            return true;
         }
      }
''')
wr("pco_nir_alu.c", s)
print("ok")
