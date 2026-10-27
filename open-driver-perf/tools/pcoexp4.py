p = "/home/radxa/mesa/mesa-main/src/imagination/pco/pco_nir.c"
s = open(p).read()
old = "   NIR_PASS(_, nir, nir_trivialize_registers);\n"
assert s.count(old) == 1, s.count(old)
new = '''   /* Timing probe: PCO_SCHED_THRESHOLD=N schedules for about N registers of pressure. */
   if (!internal && getenv("PCO_SCHED_THRESHOLD")) {
      nir_schedule_options sched_opts = {
         .threshold = atoi(getenv("PCO_SCHED_THRESHOLD")),
      };
      NIR_PASS(_, nir, nir_schedule, &sched_opts);
   }
''' + old
s = s.replace(old, new)
if '#include "nir/nir_schedule.h"' not in s:
    s = s.replace('#include "nir/nir_lower_blend.h"\n', '#include "nir/nir_lower_blend.h"\n#include "nir/nir_schedule.h"\n', 1)
if "#include <stdlib.h>" not in s:
    s = s.replace("#include <stdio.h>\n", "#include <stdio.h>\n#include <stdlib.h>\n", 1)
open(p, "w").write(s)
print('ok')
