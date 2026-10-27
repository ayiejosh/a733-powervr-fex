#!/bin/sh
# Run a GL/EGL client through the PVR Gallium wrapper (Mesa frontend -> vendor GLES 24.2).
#   . /home/radxa/gpu-open-stack/gl-wrapper-env.sh && glmark2-es2 --off-screen --size 800x600
# Non-destructive: sets env only. Needs pvrsrvkm bound (the vendor kernel driver), not powervr.
M=/home/radxa/mesa-pvr/install/lib/aarch64-linux-gnu
export LD_LIBRARY_PATH="$M:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="$M/dri"
# The loader's device->driver mapping does not fire for this platform device, so name it.
export MESA_LOADER_DRIVER_OVERRIDE=pvr
