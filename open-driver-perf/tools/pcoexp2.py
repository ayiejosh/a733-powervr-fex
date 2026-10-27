import re
R = "/home/radxa/mesa/mesa-main/src/imagination/pco/"


def rd(p): return open(R + p).read()
def wr(p, s): open(R + p, "w").write(s)


def sub(s, pat, new, flags=re.S):
    assert len(re.findall(pat, s, flags)) == 1, pat[:70]
    return re.sub(pat, lambda m: new, s, flags=flags)


s = rd("pco_legalize.c")
s = sub(s, r"      if \(prev->op == PCO_OP_FENCE\)\n         break;\n      if \(instrs_exclusive\(prev, instr\)\)\n         continue;\n",
        "      if (instrs_exclusive(prev, instr))\n         continue;\n      if (prev->op == PCO_OP_FENCE)\n         break;\n")
s = sub(s, r"(      if \(na->parent->type != PCO_CF_NODE_TYPE_IF \|\| na->flag != PCO_CF_NODE_FLAG_IF_THEN\)\n         continue;\n)",
        "      if (na->parent->type != PCO_CF_NODE_TYPE_IF || na->flag != PCO_CF_NODE_FLAG_IF_THEN)\n         continue;\n"
        "      pco_if *pif = pco_cf_node_as_if(na->parent);\n      if (!pif->uniform || pif->pred_exec)\n         continue;\n")
wr("pco_legalize.c", s)

s = rd("pco_nir_alu.c")
s = sub(s, r"      /\* With a constant, non-NaN operand second.*?\n      if \(!getenv\(\"PCO_NO_NMAX_FIX\"\)\) \{.*?\n         \}\n      \}\n",
        '''      /* With a constant, non-NaN operand the hardware compare already gives the NaN-preserving result (a NaN operand
       * makes the compare false, which selects the second operand), so the expansion is skipped. The flag stays set
       * so later passes keep the NaN semantics; trans_min_max puts the constant second.
       */
      if (alu->def.num_components == 1 && !getenv("PCO_NO_NMAX_FIX")) {
         for (unsigned i = 0; i < 2; i++) {
            nir_scalar c = nir_scalar_resolved(alu->src[i].src.ssa, alu->src[i].swizzle[0]);
            if (nir_scalar_is_const(c) && !isnan(nir_scalar_as_float(c)))
               return false;
         }
      }
''')
wr("pco_nir_alu.c", s)

s = rd("pco_trans_nir.c")
s = sub(s, r"      instr = trans_min_max\(tctx, alu->op, dest, src\[0\], src\[1\]\);\n",
        '''      /* A constant goes second: the hardware compare is false for a NaN operand, which selects the second one. */
      if ((alu->op == nir_op_fmin || alu->op == nir_op_fmax) && nir_src_is_const(alu->src[0].src) &&
          !nir_src_is_const(alu->src[1].src)) {
         pco_ref tmp_src = src[0];
         src[0] = src[1];
         src[1] = tmp_src;
      }
      instr = trans_min_max(tctx, alu->op, dest, src[0], src[1]);
''')
wr("pco_trans_nir.c", s)
print("ok")
