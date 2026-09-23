#!/bin/bash
# stacklane.sh - everything that needs the OPEN module loaded, in one boot.
#
# Two ICDs from the same source tree are compared back to back so the module,
# the clocks and the machine state are constants and the driver is the only
# variable:
#   build/     linked before the buffer-device-address and extent-limit work
#   build-x11/ linked after it, and what the system now uses by default
#
# Run under the swap:
#   sudo ./open-run.sh /home/radxa/trixie-prep/bench/pvr-vulkan/stacklane.sh
set -u
cd "$(dirname "$0")"

OLD=/home/radxa/mesa/mesa-main/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
NEW=/home/radxa/mesa/mesa-main/build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu

echo "########## Vulkan: open driver BEFORE our changes (build/, pre-BDA) ##########"
VK_ICD_FILENAMES=$OLD VK_DRIVER_FILES=$OLD ./stackbench.sh open-prechange
echo
echo "########## Vulkan: open driver NOW (build-x11, default config) ##########"
VK_ICD_FILENAMES=$NEW VK_DRIVER_FILES=$NEW ./stackbench.sh open-current

echo
echo "########## GL: glbench 1280x720 through zink over each ICD ##########"
# Frame counts fall with the shader loop: loop256 is ~64x the ALU of loop4, so a
# fixed count would turn the tail of this into a coffee break.
for pair in "prechange:$OLD" "current:$NEW"; do
  label=${pair%%:*}; path=${pair#*:}
  for L in 4 16 64 256; do
    case $L in 4) F=120 ;; 16) F=60 ;; 64) F=20 ;; *) F=6 ;; esac
    printf 'gl.%-10s loop%-4s ' "$label" "$L"
    LD_LIBRARY_PATH=$GL_PREFIX LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
      GBM_BACKENDS_PATH=$GL_PREFIX/gbm MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
      DRM_RENDER_NODE=/dev/dri/renderD128 MESA_GLES_VERSION_OVERRIDE=3.2 \
      VK_ICD_FILENAMES=$path VK_DRIVER_FILES=$path PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
      timeout 600 /tmp/glbench /dev/dri/renderD128 $L $F 2>&1 |
      grep -E "GL_RENDERER|RESULT" | tr '\n' ' '
    echo
  done
done

echo
echo "########## presentation: dma-buf scanout with page flips ##########"
for pair in "prechange:$OLD" "current:$NEW"; do
  label=${pair%%:*}; path=${pair#*:}
  for spec in "1920 1080 240" "3840 2160 120"; do
    printf 'present.%-10s %-10s ' "$label" "$(echo "$spec" | cut -d' ' -f1)"
    VK_ICD_FILENAMES=$path VK_DRIVER_FILES=$path PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
      timeout 600 ./pvranimate $spec 2>&1 |
      grep -E "presented|VERDICT|timing ms/frame" | tr '\n' ' '
    echo
  done
done
echo
echo "########## done ##########"
