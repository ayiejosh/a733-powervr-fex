#!/bin/sh
# Run glmark2 on the OPEN PowerVR stack through the working frontend: Wayland (headless weston) with
# zink -> open Vulkan. The X11/Xwayland frontend currently dies on this stack because the open DRM
# node does not support the dumb buffers Xwayland wants (DRM_IOCTL_MODE_CREATE_DUMB: Permission
# denied), so Wayland is the path that works and is much faster.
#
# Needs: glmark2-es2-wayland (Debian's .deb can be extracted without installing, see below),
# weston, and the open driver loaded with the built Mesa (open-perf/open-enter.sh).
#
# Usage: bench/glmark2-wayland-open.sh OUT [glmark2 args...]
OUT=${1:-/tmp/glmark2-open-wayland.txt}; shift 2>/dev/null || true
GLM=/tmp/glm2w/usr/bin/glmark2-es2-wayland
if [ ! -x "$GLM" ]; then
   # Extract the Debian wayland frontend locally (no system install; apt deps on this image are broken).
   curl -sL -o /tmp/glm2w.deb "http://deb.debian.org/debian/pool/main/g/glmark2/glmark2-es2-wayland_2023.01+dfsg-3+b1_arm64.deb" &&
      dpkg-deb -x /tmp/glm2w.deb /tmp/glm2w
fi
[ -x "$GLM" ] || { echo "glmark2-es2-wayland unavailable"; exit 1; }

. /home/radxa/gpu-open-stack/env26.sh
export XDG_RUNTIME_DIR=/tmp/wroot
rm -rf /tmp/wroot; mkdir -p /tmp/wroot; chmod 700 /tmp/wroot
printf "[core]\nrequire-input=false\n" > /tmp/wsmoke.ini
weston --config=/tmp/wsmoke.ini --backend=headless-backend.so --renderer=gl \
   --socket=wayland-open --idle-time=0 --log=/tmp/glmark-weston.log >/dev/null 2>&1 &
W=$!
sleep 3
export WAYLAND_DISPLAY=wayland-open
timeout 900 env $CLIENT_ENV "$GLM" --size 800x600 "$@" > "$OUT" 2>&1
rc=$?
kill $W 2>/dev/null
grep -E "glmark2 Score|GL_RENDERER" "$OUT" | tail -2
echo "glmark2 rc=$rc (log: $OUT)"
