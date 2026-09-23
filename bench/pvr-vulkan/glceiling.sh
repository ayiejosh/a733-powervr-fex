#!/bin/bash
# glceiling.sh - what GL version does zink actually reach on this ICD?
#
# The regression suite runs glheadless with MESA_GLES_VERSION_OVERRIDE=3.2, which
# *forces* the reported version and therefore hides the real ceiling. This runs
# the same binary with the same environment but without the override, so
# glheadless's own context attempts (GLES 3 major/minor, GLES 3 client version,
# GLES 2) report which one the stack actually grants.
#
# Compare:
#   vendor + the PVR_strip layer : GLES 2.0 (GLES 3 rejected, EGL_BAD_MATCH),
#                                  zink's only complaint is fillModeNonSolid
#   open driver, no layer        : this script
#
# The WARNING line is the point of the run, not noise: zink names the base
# requirements it finds missing, which is the shortest path to "what would have to
# be true for the ceiling to move".
#
# Run under the open driver via kernel/open-driver-spike/open-run.sh; it needs no
# display and no X server.
set -u
cd "$(dirname "$0")"

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
SIZE=${1:-512}
FRAMES=${2:-3}

# Deliberately NOT set: MESA_GLES_VERSION_OVERRIDE.
env -u MESA_GLES_VERSION_OVERRIDE \
    LD_LIBRARY_PATH="$GL_PREFIX" \
    LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
    GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
    MESA_LOADER_DRIVER_OVERRIDE=zink \
    GALLIUM_DRIVER=zink \
    EGL_PLATFORM=gbm \
    DRM_RENDER_NODE=/dev/dri/renderD128 \
    timeout 300 ./glheadless "$SIZE" "$FRAMES" 2>&1 |
  grep -E "EGL |context |GL_VENDOR|GL_RENDERER|GL_VERSION|GLSL|RESULT|VERDICT|error|WARNING|base Zink" |
  sed 's/^/  /'
