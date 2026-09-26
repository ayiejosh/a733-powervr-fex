#!/bin/bash
# stackdetail.sh - the gaps left by stacklane.sh, in one boot of the open module.
#
# 1. what bda/pctest do on the pre-change driver (the "it did not work before"
#    evidence, captured rather than inferred)
# 2. memtypes raw for both open ICDs, because the parsed row came back empty
# 3. GL through zink over the open driver, surfaceless - the GBM/pbuffer config
#    the first attempt used does not exist under zink, which is why it printed
#    nothing at all rather than failing loudly
# 4. vkrender 512 with more frames, to check whether the pre-change driver really
#    is the slower of the two or that was noise
set -u
cd "$(dirname "$0")"

OLD=/home/radxa/mesa/mesa-main/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
NEW=/home/radxa/mesa/mesa-main/build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu

echo "########## 1. the compatibility tests on the pre-change driver ##########"
for t in bda pctest; do
  echo "--- $t, pre-change ICD ---"
  VK_ICD_FILENAMES=$OLD VK_DRIVER_FILES=$OLD PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    timeout 120 ./$t > /tmp/detail-$t.log 2>&1
  echo "    exit=$?  (last 4 lines)"
  tail -4 /tmp/detail-$t.log | sed 's/^/    /'
done

echo
echo "########## 2. memtypes raw, both open ICDs ##########"
for pair in "prechange:$OLD" "current:$NEW"; do
  echo "--- ${pair%%:*} ---"
  VK_ICD_FILENAMES=${pair#*:} VK_DRIVER_FILES=${pair#*:} PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    timeout 300 ./memtypes 2>&1 | tail -8
done

echo
echo "########## 3. GL through zink over the OPEN driver, surfaceless ##########"
for L in 4 16 64 256; do
  case $L in 4) F=60 ;; 16) F=40 ;; 64) F=20 ;; *) F=8 ;; esac
  printf 'gl.zink-pvr loop%-4s ' "$L"
  LD_LIBRARY_PATH=$GL_PREFIX LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
    GBM_BACKENDS_PATH=$GL_PREFIX/gbm MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
    MESA_GLES_VERSION_OVERRIDE=3.2 \
    VK_ICD_FILENAMES=$NEW VK_DRIVER_FILES=$NEW PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    timeout 600 /tmp/glbench none $L $F 2>&1 | grep -E "GL_RENDERER|RESULT" | tr '\n' ' '
  echo
done

echo
echo "########## 4. vkrender 512, 60 frames, twice each ##########"
for pair in "prechange:$OLD" "current:$NEW"; do
  for i in 1 2; do
    printf 'render512.%-10s #%d  ' "${pair%%:*}" "$i"
    VK_ICD_FILENAMES=${pair#*:} VK_DRIVER_FILES=${pair#*:} PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
      timeout 600 ./vkrender 512 60 2>&1 | grep -E "ms/frame" | head -1
  done
done
echo
echo "########## done ##########"
