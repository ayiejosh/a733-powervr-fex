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
#   glheadless  EGL surfaceless/device + zink: desktop GL (or GLES3) over the Vulkan ICD
#   pvrscanout  render -> export dma-buf -> import into sunxi-drm -> scan it out
#   pvranimate  the same, but double buffered and presented with page flips
#
# The Vulkan loader is linked directly; on this board the arm64 loader has no .so
# symlink in the default path, so fall back to linking the versioned library.
set -eu
cd "$(dirname "$0")"

glslangValidator -V --target-env vulkan1.1 -o compute.spv compute.comp
glslangValidator -V --target-env vulkan1.1 -o render_vert.spv render.vert
glslangValidator -V --target-env vulkan1.1 -o render_frag.spv render.frag
glslangValidator -V --target-env vulkan1.1 -o anim_frag.spv anim.frag

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
emit('anim_frag.spv', 'anim_frag_spv', 'anim_frag_spv.h')
PY

CC=${CC:-gcc}
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter"

# Compile, then fall back to the versioned loader only if the *link* failed.
# Errors must be visible: a swallowed compile error leaves a stale binary behind,
# which silently invalidates whatever test run follows.
link() {
  local out=$1 src=$2
  if ! $CC $CFLAGS -o "$out" "$src" -lvulkan -lm 2>/tmp/link.err; then
    if grep -q 'cannot find -lvulkan' /tmp/link.err; then
      $CC $CFLAGS -o "$out" "$src" -l:libvulkan.so.1 -lm
    else
      cat /tmp/link.err >&2
      echo "build.sh: $src failed to compile" >&2
      exit 1
    fi
  fi
}

link vktest vktest.c
link vkrender vkrender.c

# memtypes: what memory the driver offers and how fast a readback is on it.
link memtypes memtypes.c

# vkaudit: diffable capability dump, for comparing two ICDs.
link vkaudit vkaudit.c

# pvrscanout also needs libdrm for the KMS/PRIME half.
$CC $CFLAGS -I/usr/include/libdrm -o pvrscanout pvrscanout.c -lvulkan -ldrm -lm 2>/dev/null || \
  $CC $CFLAGS -I/usr/include/libdrm -o pvrscanout pvrscanout.c -l:libvulkan.so.1 -ldrm -lm

# pvranimate: the same two devices, but presenting continuously with page flips.
$CC $CFLAGS -I/usr/include/libdrm -o pvranimate pvranimate.c -lvulkan -ldrm -lm 2>/dev/null || \
  $CC $CFLAGS -I/usr/include/libdrm -o pvranimate pvranimate.c -l:libvulkan.so.1 -ldrm -lm

# glheadless links EGL + GLES2; the Mesa build that provides zink is separate
# (mesa/build-gl) and is selected at run time with LD_LIBRARY_PATH.
glheadless_link() {
  if ! $CC $CFLAGS -o glheadless glheadless.c -lEGL -lGLESv2 -lgbm -lm 2>/tmp/link.err; then
    if grep -q 'cannot find -l' /tmp/link.err; then
      $CC $CFLAGS -o glheadless glheadless.c -l:libEGL.so.1 -l:libGLESv2.so.2 -l:libgbm.so.1 -lm
    else
      cat /tmp/link.err >&2
      echo "build.sh: glheadless.c failed to compile" >&2
      exit 1
    fi
  fi
}
glheadless_link

echo "built: $(pwd)/vktest, $(pwd)/vkrender and $(pwd)/glheadless"
