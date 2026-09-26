#!/bin/sh
# X11: GPU OpenGL is delivered by zink -> PowerVR Vulkan.
# Why: the vendor DDK is GLES-only and the vendor Xorg exports no GLX, so Qt's EGL config
# chooser - which asks for EGL_OPENGL_BIT (desktop GL) - finds ZERO matching configs on the
# vendor EGL and aborts every GL path with "Cannot find EGLConfig, returning null config".
# System Mesa + zink answers the same request with 45 configs, backed by the PowerVR through
# the IMG Vulkan ICD. Verified 2026-09-22 - see docs/GPU-RESEARCH-2026-09-22.md.
# Revert: restore 10-software-render.sh.bak-forced-software-20260922 (forces software GL).
if [ "$XDG_SESSION_TYPE" != "wayland" ]; then
  # system Mesa must win over the vendor stack in /usr/local/lib (ld.so.conf priority)
  export LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  export LIBGL_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
  export LIBGL_KOPPER_DRI2=1   # Radxa task-a733-powervr sets this; without it Mesa EGL cannot make a dri2 screen here
  export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink
  export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json
  export VK_LAYER_PATH=/home/radxa/gpu-experiment
  export VK_INSTANCE_LAYERS=VK_LAYER_PVR_strip
  # zink on IMG requires geometryShader and fillModeNonSolid; our layer fakes both for the
  # capability check and strips them at device create, so the blob still accepts the device
  export PVR_FAKE_GS=1 PVR_FAKE_FILL=1
  # the vendor Xorg exports no GLX; skip Qt's GLX attempts entirely
  export QT_XCB_GL_INTEGRATION=xcb_egl
  # the shell itself stays on the software backend for now (stage 2 tries it on the GPU)
  export QT_QUICK_BACKEND=opengl   # stage 2: shell on the GPU via zink
fi
