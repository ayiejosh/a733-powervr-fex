#!/bin/sh
# build-ap-mesa.sh OUT_DIR - build vanilla mesa-26.3.0-rc2 + the apercel transform-feedback patch
# (GL 3.2 core / GLES 3.1 via zink, VK_EXT_transform_feedback). Needs the mesa repo (tag fetched) and the
# apercel repo (~/apercel). The patched tree renames the DRM driver match to "apercel"; change
# PVR_DRM_DRIVER_NAME in src/imagination/vulkan/winsys/pvr_winsys.h to "powervr" to run it on the project's
# kernel driver instead of apercel.ko.
set -eu
OUT=${1:?usage: build-ap-mesa.sh OUT_DIR}
MESA=${MESA_REPO:-/home/radxa/mesa/mesa-main}
AP=${APERCEL_REPO:-/home/radxa/apercel}
WORK=$(dirname "$OUT")/mesa-ap-src
[ -d "$WORK" ] || git -C "$MESA" worktree add -f "$WORK" mesa-26.3.0-rc2
[ -f /tmp/ap263.patch ] || git -C "$AP" show origin/mesa-26.3:mesa/apercel-mesa-26.3.0-rc2.patch > /tmp/ap263.patch
(cd "$WORK" && git apply /tmp/ap263.patch)
meson setup "$OUT" "$WORK" --prefix=/usr/local -Dbuildtype=release \
   -Dvulkan-drivers=imagination -Dgallium-drivers=zink -Dopengl=true -Dgles2=enabled \
   -Dplatforms=x11,wayland -Dglx=dri -Dshader-cache=enabled
ninja -C "$OUT" -j6
echo "built: $OUT"
echo "ICD (dev-env, points at the build tree): $OUT/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json"
