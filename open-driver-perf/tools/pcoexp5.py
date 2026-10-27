p = "/home/radxa/mesa/mesa-main/src/imagination/pco/pco_nir.c"
s = open(p).read()
old = '''      nir_schedule_options sched_opts = {
         .threshold = atoi(getenv("PCO_SCHED_THRESHOLD")),
      };'''
assert s.count(old) == 1, s.count(old)
new = '''      nir_schedule_options sched_opts = {
         .threshold = atoi(getenv("PCO_SCHED_THRESHOLD")),
      };
      if (getenv("PCO_SCHED_DELAYS"))
         sched_opts.instr_delay_cb = pco_sched_delay_cb;'''
s = s.replace(old, new)
anchor = "void pco_postprocess_nir(pco_ctx *ctx, nir_shader *nir, pco_data *data)\n"
assert s.count(anchor) == 1
cb = '''/* Timing probe: rough latencies in instruction groups for the NIR scheduler. */
static unsigned pco_sched_delay_cb(nir_instr *instr, void *data)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      switch (nir_instr_as_alu(instr)->op) {
      case nir_op_frcp:
      case nir_op_frsq:
      case nir_op_flog2:
      case nir_op_fexp2:
      case nir_op_fsin:
      case nir_op_fcos:
         return 8;
      default:
         return 1;
      }

   case nir_instr_type_intrinsic:
      switch (nir_instr_as_intrinsic(instr)->intrinsic) {
      case nir_intrinsic_smp_pco:
         return 32;
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ssbo:
      case nir_intrinsic_load_global:
         return 24;
      case nir_intrinsic_load_input:
         return 6;
      default:
         return 1;
      }

   default:
      return 1;
   }
}

'''
s = s.replace(anchor, cb + anchor)
open(p, "w").write(s)
print("ok")
