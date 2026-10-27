import re
p = "/home/radxa/mesa/mesa-main/src/imagination/pco/pco_nir_lower_null_descriptors.c"
s = open(p).read()
old = "   nir_if *nif = nir_push_if(&b, slow);\n"
assert s.count(old) == 1, s.count(old)
new = "   /* Timing probe only: PCO_FORCE_FAST compiles just the unchecked copy. */\n   if (getenv(\"PCO_FORCE_FAST\"))\n      slow = nir_imm_false(&b);\n" + old
s = s.replace(old, new)
if "#include <stdlib.h>" not in s:
    s = s.replace('#include "pco_internal.h"\n', '#include "pco_internal.h"\n#include <stdlib.h>\n', 1)
open(p, "w").write(s)
print("ok", "#include <stdlib.h>" in s)
