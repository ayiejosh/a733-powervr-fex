import re, subprocess, sys
R = sys.argv[1].rstrip("/") + "/src/imagination/pco/"


def rd(p): return open(R + p).read()
def wr(p, s): open(R + p, "w").write(s)


def sub(s, pat, new, flags=re.S):
    assert len(re.findall(pat, s, flags)) == 1, pat[:70]
    return re.sub(pat, lambda m: new.replace(chr(92) + "1", m.group(0)), s, flags=flags)


def commit(msg, files):
    subprocess.run(["git", "-C", sys.argv[1], "add"] + ["src/imagination/pco/" + f for f in files], check=True)
    n = subprocess.run(["git", "-C", sys.argv[1], "log", "-1", "--format=%an"], capture_output=True, text=True, check=True).stdout.strip()
    e = subprocess.run(["git", "-C", sys.argv[1], "log", "-1", "--format=%ae"], capture_output=True, text=True, check=True).stdout.strip()
    subprocess.run(["git", "-C", sys.argv[1], "-c", "user.name=" + n, "-c", "user.email=" + e, "commit", "-q", "-m", msg], check=True)


# ---- A: pco_shrink_vecs ----
s = rd("pco_opt.c")
s = sub(s, r"         /\* Can't shrink if has collated elems\. \*/\n         if \(!only_uncollated_elems\(vec\)\)\n            continue;\n",
        "         bool collated = !only_uncollated_elems(vec);\n")
s = sub(s, r"         /\* Whole vec used, skip\. \*/\n         if \(chans_used == chans\)\n            continue;\n\n?         /\* Update the vec\. \*/\n         vec->num_srcs = chans_used;\n         \*pdest = pco_ref_chans\(\*pdest, chans_used\);\n\n?         progress = true;\n",
        '''         /* Whole vec used, skip. */
         if (chans_used == chans)
            continue;

         if (collated) {
            /* Only whole sources can be dropped: keep the ones that start
             * before the end of the last used channel.
             */
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
s = sub(s, r"         /\* Has collated vec, skip\. \*/\n         /\* TODO: support this\. \*/\n         if \(vec->num_srcs != util_dynarray_num_elements\(&comps, pco_instr \*\)\) \{",
        """         /* Has collated vec, skip. */
         /* TODO: support this. */
         if (!only_uncollated_elems(vec) ||
             vec->num_srcs != util_dynarray_num_elements(&comps, pco_instr *)) {""")
wr("pco_opt.c", s)
commit("""pco: shrink vecs that have collated sources

pco_shrink_vecs only trimmed vecs made of single channel sources. A vec
that mixes in a multi channel source, such as the result of a varying
iteration used as texture coordinates, kept all of its dead trailing
channels, each one a zero copy and a temporary: the coordinate vec of a
texture sample was 16 channels wide for 4 used ones.

Drop the trailing sources that start after the last used channel, whole
sources only.

effect2d goes from 31 to 16 temporaries and 58 to 52 groups, bump from 18
to 16 temporaries and 233 to 218 groups. glmark2 through zink, off-screen:
effect2d +15%, desktop shadow +16%, bump +3%.

pco_opt_comp_only_vecs counted sources to recognise vecs without collated\nelements; a shrunk collated vec can now match that count, so check it\nexplicitly.""", ["pco_opt.c"])

# ---- B: min/max with a constant operand ----
s = rd("pco_nir_alu.c")
if "#include <math.h>" not in s:
    s = s.replace('#include "pco_internal.h"\n', '#include "pco_internal.h"\n\n#include <math.h>\n', 1)
s = sub(s, r"(      if \(!nir_alu_instr_is_nan_preserve\(alu\)\)\n         break;\n)",
        r"\1" + '''
      /* With a constant, non-NaN operand the hardware compare already gives
       * the NaN preserving result (a NaN operand makes the compare false,
       * which selects the second operand), so the expansion is skipped. The
       * flag stays set so later passes keep the NaN semantics, and
       * trans_min_max puts the constant second.
       */
      if (alu->def.num_components == 1) {
         for (unsigned i = 0; i < 2; i++) {
            nir_scalar c = nir_scalar_resolved(alu->src[i].src.ssa,
                                               alu->src[i].swizzle[0]);
            if (nir_scalar_is_const(c) && !isnan(nir_scalar_as_float(c)))
               return false;
         }
      }

''')
wr("pco_nir_alu.c", s)
s = rd("pco_trans_nir.c")
s = sub(s, r"(static pco_instr \*trans_min_max\(trans_ctx \*tctx,)", """/* Whether an ALU source resolves to a constant that is not NaN. */
static bool alu_src_is_nonnan_const(const nir_alu_instr *alu, unsigned i)
{
   nir_scalar c =
      nir_scalar_resolved(alu->src[i].src.ssa, alu->src[i].swizzle[0]);
   return nir_scalar_is_const(c) && !isnan(nir_scalar_as_float(c));
}

""" + chr(92) + "1")
if "#include <math.h>" not in s:
    s = s.replace('#include "pco_internal.h"\n', '#include "pco_internal.h"\n\n#include <math.h>\n', 1)
s = sub(s, r"      instr = trans_min_max\(tctx, alu->op, dest, src\[0\], src\[1\]\);\n",
        '''      /* A constant goes second: the hardware compare is false for a NaN
       * operand, which selects the second one.
       */
      if ((alu->op == nir_op_fmin || alu->op == nir_op_fmax) &&
          alu_src_is_nonnan_const(alu, 0) && !alu_src_is_nonnan_const(alu, 1)) {
         pco_ref tmp_src = src[0];
         src[0] = src[1];
         src[1] = tmp_src;
      }

      instr = trans_min_max(tctx, alu->op, dest, src[0], src[1]);
''')
wr("pco_trans_nir.c", s)
commit("""pco: skip the NaN expansion of fmin/fmax with a constant operand

All hardware comparisons are false when an operand is NaN, so NaN
preserving fmin/fmax was expanded to a min/max plus two isnan selects:
three serial groups, for example for max(x, 0.0) in a lighting shader.

With a non NaN constant as the second operand the compare already gives
the NaN preserving result: a NaN in the other operand makes it false and
the constant is selected. Skip the expansion for scalar min/max with such
a constant, keep the preserve flag so later passes (for example the
min/max to fsat patterns) still see the NaN semantics, and put the
constant second when translating.

shading phong goes from 52 to 48 groups and 15 to 14 temporaries.
glmark2 through zink, off-screen: +1% to +3% on shading, bump, ideas,
texture and shadow.""", ["pco_nir_alu.c", "pco_trans_nir.c"])

# ---- C: pre-fence across exclusive branches ----
s = rd("pco_legalize.c")
helper = '''/**
 * \\brief Check whether two instructions are in the then and the else body of
 * the same uniform if, so that they never run on the same path.
 *
 * \\param[in] a First instruction.
 * \\param[in] b Second instruction.
 * \\return True if the instructions are mutually exclusive.
 */
static bool instrs_exclusive(pco_instr *a, pco_instr *b)
{
   for (pco_cf_node *na = &a->parent_block->cf_node; na->parent;
        na = na->parent) {
      if (na->parent->type != PCO_CF_NODE_TYPE_IF ||
          na->flag != PCO_CF_NODE_FLAG_IF_THEN) {
         continue;
      }

      /* Divergent ifs issue both bodies back to back. */
      pco_if *pif = pco_cf_node_as_if(na->parent);
      if (!pif->uniform || pif->pred_exec)
         continue;

      for (pco_cf_node *nb = &b->parent_block->cf_node; nb->parent;
           nb = nb->parent) {
         if (nb->parent != na->parent || nb->flag != PCO_CF_NODE_FLAG_IF_ELSE)
            continue;

         /* In a loop the bodies can run in successive iterations. */
         for (pco_cf_node *up = na->parent->parent; up; up = up->parent) {
            if (up->type == PCO_CF_NODE_TYPE_LOOP)
               return false;
         }

         return true;
      }
   }

   return false;
}

'''
s = sub(s, r"(/\*\*\n \* \\brief Check whether a DITR/DITRP instruction requires a pre-fence\.)", helper + chr(92) + "1")
s = sub(s, r"      if \(prev->op == PCO_OP_FENCE\)\n         break;\n",
        "      /* Instructions on the other path of an if do not run before this one. */\n      if (instrs_exclusive(prev, instr))\n         continue;\n\n      if (prev->op == PCO_OP_FENCE)\n         break;\n")
wr("pco_legalize.c", s)
commit("""pco: ignore the other branch of a uniform if for DITR pre-fences

ditr_needs_pre_fence walks the instruction list backwards looking for
uses of the iterator destination registers. In a versioned shader the
walk crosses from the unchecked copy (else) into the checked copy (then),
which never runs on the same path, and finds registers in common, so the
unchecked copy got a pre-fence of three groups it does not need.

Skip instructions in the then body when the DITR is in the else body of
the same uniform if. The skip comes before the fence check, as a fence
in the other body does not cover this path, and is limited to ifs that
are neither predicated nor inside a loop.

jellyfish and bump lose three groups on the executed path.""", ["pco_legalize.c"])
print("ok")
