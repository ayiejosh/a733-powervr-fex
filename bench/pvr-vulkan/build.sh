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
glslangValidator -V --target-env vulkan1.1 -o pc.spv pc.comp
glslangValidator -V --target-env vulkan1.1 -o pc64.spv pc64.comp
# bda_*.comp need SPIR-V 1.3 + PhysicalStorageBufferAddresses, so vulkan1.2.
glslangValidator -V --target-env vulkan1.2 -o bda_pc.spv bda_pc.comp
glslangValidator -V --target-env vulkan1.2 -o bda_ubo.spv bda_ubo.comp
# vk13 tests the Vulkan 1.3 features (zero-init workgroup memory keeps its
# uninitialised shared variable, which needs no special environment).
glslangValidator -V --target-env vulkan1.1 -o zero_shared.spv zero_shared.comp
glslangValidator -V --target-env vulkan1.1 -o robust_image.spv robust_image.comp
# f16_alu needs the explicit narrow arithmetic types, which are Vulkan 1.1 +
# the SPIR-V 1.3 features glslang enables for this target.
glslangValidator -V --target-env vulkan1.1 -o f16_alu.spv f16_alu.comp
glslangValidator -V --target-env vulkan1.1 -o bits_storage.spv bits_storage.comp
# io16: storageInputOutput16 - the same triangle, but the blue channel reaches the
# fragment stage through a 16-bit varying, so the expected image is unchanged.
glslangValidator -V --target-env vulkan1.1 -o io16.vert.spv io16.vert
glslangValidator -V --target-env vulkan1.1 -o io16.frag.spv io16.frag
glslangValidator -V --target-env vulkan1.1 -o desc_idx.spv desc_idx.comp
# desc_nonuniform: the same descriptor array as desc_idx, but indexed by a value
# that diverges between lanes of one wave (uniform control in the same binary).
glslangValidator -V --target-env vulkan1.1 -o desc_nonuniform.spv desc_nonuniform.comp
# dc.vert: the depth clamp probe - render.vert's triangle at z outside the clip
# volume, so the fragment is either clipped or clamped depending on the state.
glslangValidator -V --target-env vulkan1.1 -o dc.vert.spv dc.vert
# vsstore.vert: vertexPipelineStoresAndAtomics - the same triangle, plus an SSBO
# store and an atomic add from the vertex stage.
glslangValidator -V --target-env vulkan1.1 -o vsstore.vert.spv vsstore.vert

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
emit('pc.spv', 'pc_spv', 'pc_spv.h')
emit('pc64.spv', 'pc64_spv', 'pc64_spv.h')
emit('bda_pc.spv', 'bda_pc_spv', 'bda_pc_spv.h')
emit('bda_ubo.spv', 'bda_ubo_spv', 'bda_ubo_spv.h')
emit('zero_shared.spv', 'zero_shared_spv', 'zero_shared_spv.h')
emit('robust_image.spv', 'robust_image_spv', 'robust_image_spv.h')
emit('f16_alu.spv', 'f16_alu_spv', 'f16_alu_spv.h')
emit('bits_storage.spv', 'bits_storage_spv', 'bits_storage_spv.h')
emit('io16.vert.spv', 'io16_vert_spv', 'io16_vert_spv.h')
emit('io16.frag.spv', 'io16_frag_spv', 'io16_frag_spv.h')
emit('desc_idx.spv', 'desc_idx_spv', 'desc_idx_spv.h')
emit('desc_nonuniform.spv', 'desc_nonuniform_spv', 'desc_nonuniform_spv.h')
emit('dc.vert.spv', 'dc_vert_spv', 'dc_vert_spv.h')
emit('vsstore.vert.spv', 'vsstore_vert_spv', 'vsstore_vert_spv.h')
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

# bda: VK_KHR_buffer_device_address functional test (raw address delivered by a
# push constant and by a uniform buffer; store, load+store and global atomic).
link bda bda.c

# pctest: minimal vkCmdPushConstants probe, with no buffer device address in it.
link pctest pctest.c

# vk13: the three Vulkan 1.3 features section 21.16 implements.
link vk13 vk13.c

# vk16: narrow-type (f16/i8) arithmetic probe.
link vk16 vk16.c

# vkbits: 8/16-bit storage access, with sentinels that catch a store that
# clobbers its neighbours.
link vkbits vkbits.c

# vkdescidx: descriptor array indexed by a dynamically-uniform value.
link vkdescidx vkdescidx.c

# vkdescnon: the same array indexed by a per-lane DIVERGENT value, with the
# dynamically-uniform case as an in-binary control so a failure is unambiguous.
link vkdescnon vkdescnon.c

# x11present: X11 WSI end to end - xcb surface, swapchain, present, then ask the X
# server what it is displaying. Needs libxcb and a DRI3-capable X server; exits 3
# (SKIP) with the surface facts when the server has no DRI3, as Xvfb does not.
if ! $CC $CFLAGS -o x11present x11present.c -lvulkan -lxcb 2>/tmp/link.err; then
  cat /tmp/link.err >&2
  echo "build.sh: x11present.c failed to compile" >&2
  exit 1
fi

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
