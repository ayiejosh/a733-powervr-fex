#!/bin/bash
# build.sh - compile the PowerVR Vulkan tests.
#
# Shaders are compiled to SPIR-V with glslangValidator and embedded as uint32_t
# words (vkCreateShaderModule requires 4-byte alignment, which a char array does
# not promise). Written in python because this board has no xxd.
#
# Two binaries:
#   vktest    compute execution + readback verification + throughput
#   vkrender  offscreen graphics: render pass, draw, resolve, copy, pixel check
#
# The Vulkan loader is linked directly; on this board the arm64 loader has no .so
# symlink in the default path, so fall back to linking the versioned library.
set -eu
cd "$(dirname "$0")"

glslangValidator -V --target-env vulkan1.1 -o compute.spv compute.comp
glslangValidator -V --target-env vulkan1.1 -o render_vert.spv render.vert
glslangValidator -V --target-env vulkan1.1 -o render_frag.spv render.frag

python3 - <<'PY'
import struct

def emit(spv, name, out):
    data = open(spv, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    with open(out, 'w') as f:
        f.write('/* generated from %s by build.sh - do not edit */\n' % spv)
        f.write('static const unsigned int %s[] = {\n' % name)
        for i in range(0, len(words), 8):
            f.write('    ' + ', '.join('0x%08xu' % w for w in words[i:i + 8]) + ',\n')
        f.write('};\n')
    print('embedded %-16s %5d bytes of SPIR-V' % (spv, len(data)))

emit('compute.spv', 'compute_spv', 'compute_spv.h')
emit('render_vert.spv', 'render_vert_spv', 'render_vert_spv.h')
emit('render_frag.spv', 'render_frag_spv', 'render_frag_spv.h')
PY

CC=${CC:-gcc}
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter"

link() {
  local out=$1 src=$2
  if $CC $CFLAGS -o "$out" "$src" -lvulkan -lm 2>/dev/null; then
    :
  else
    $CC $CFLAGS -o "$out" "$src" -l:libvulkan.so.1 -lm
  fi
}

link vktest vktest.c
link vkrender vkrender.c

echo "built: $(pwd)/vktest and $(pwd)/vkrender"
