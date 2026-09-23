#!/bin/bash
# build.sh - compile the PowerVR Vulkan execution test.
#
# Two steps that matter: the compute shader is compiled to SPIR-V with
# glslangValidator and embedded into the binary as uint32 words, so the test is a single
# self-contained executable with no shader files to lose. The Vulkan loader is
# linked directly; on this board the arm64 loader has no .so symlink in the
# default path, so fall back to linking the versioned library.
set -eu
cd "$(dirname "$0")"

glslangValidator -V --target-env vulkan1.1 -o compute.spv compute.comp

# Embed as uint32_t words, not a byte array: the code passed to
# vkCreateShaderModule must be 4-byte aligned, and a char array does not promise
# that. Written in python because this board has no xxd.
python3 - <<'PY'
import struct
data = open('compute.spv', 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
with open('compute_spv.h', 'w') as f:
    f.write('/* generated from compute.spv by build.sh - do not edit */\n')
    f.write('static const unsigned int compute_spv[] = {\n')
    for i in range(0, len(words), 8):
        f.write('    ' + ', '.join('0x%08xu' % w for w in words[i:i + 8]) + ',\n')
    f.write('};\n')
print('embedded %d bytes of SPIR-V' % len(data))
PY

CC=${CC:-gcc}
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter"

if $CC $CFLAGS -o vktest vktest.c -lvulkan 2>/dev/null; then
  :
else
  $CC $CFLAGS -o vktest vktest.c -l:libvulkan.so.1
fi

echo "built: $(pwd)/vktest"
