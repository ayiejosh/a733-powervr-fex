#!/bin/bash
# complete-fex-env.sh : one-shot completion of the FEX x86 runtime environment.
# Closes the coverage gaps found by the 2026-06-11 four-domain audit so that real
# 32-bit/64-bit x86 apps (games, Steam, SDL/OpenAL apps) don't hit missing-component
# crashes one at a time. Idempotent: re-running just confirms already-installed pkgs.
#   - i386 app-runtime base (SDL1/2 family, OpenAL, codecs, curl, X session libs, GLU, usb)
#   - software Vulkan for i386 (lavapipe) -> fixes Steam "vulkan instance -9"
#   - audio bridge (ALSA->Pulse plugin both arches) + asound.conf
#   - fonts top-up
# Run as: sudo /home/radxa/complete-fex-env.sh   (re-execs itself with sudo)
set -u
ROOT=/home/radxa/crd-rootfs
if [ "$(id -u)" -ne 0 ]; then exec sudo -E "$0" "$@"; fi

echo ">>> [1/4] preparing chroot mounts + dns..."
rm -f "$ROOT/etc/resolv.conf"; printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > "$ROOT/etc/resolv.conf"
for m in proc sys dev dev/pts; do
  mkdir -p "$ROOT/$m"
  mountpoint -q "$ROOT/$m" || mount --rbind "/$m" "$ROOT/$m" 2>/dev/null || true
  mount --make-rslave "$ROOT/$m" 2>/dev/null || true
done
mkdir -p "$ROOT/run"; mountpoint -q "$ROOT/run" || mount -t tmpfs tmpfs "$ROOT/run" 2>/dev/null || true

echo ">>> [2/4] apt update (with i386 indexes; sandbox-user=root workaround for FEX gpgv)..."
chroot "$ROOT" /bin/bash --login -c "
  export DEBIAN_FRONTEND=noninteractive
  apt-get -o APT::Sandbox::User=root update 2>&1 | tail -4
"

echo ">>> [3/4] installing the completion package set (this is slow under FEX, be patient)..."
chroot "$ROOT" /bin/bash --login -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get -o APT::Sandbox::User=root install -y --no-install-recommends \
    `# --- graphics: GLU + libOpenGL convenience (both arches) + i386 software Vulkan (lavapipe) ---` \
    libglu1-mesa libglu1-mesa:i386 libopengl0 libopengl0:i386 mesa-vulkan-drivers:i386 \
    `# --- audio: ALSA->Pulse plugin (both arches) + glib mainloop + openal data ---` \
    libasound2-plugins libasound2-plugins:i386 libpulse-mainloop-glib0:i386 \
    libopenal-data libopenal1:i386 \
    `# --- SDL families (huge fraction of Linux games) ---` \
    libsdl2-2.0-0:i386 libsdl2-image-2.0-0:i386 libsdl2-mixer-2.0-0:i386 \
    libsdl2-ttf-2.0-0:i386 libsdl2-net-2.0-0:i386 \
    libsdl1.2debian:i386 libsdl-image1.2:i386 libsdl-mixer1.2:i386 libsdl-ttf2.0-0:i386 \
    `# --- audio/video codecs commonly dlopen-ed ---` \
    libvorbisfile3:i386 libtheora0:i386 libspeex1:i386 libsamplerate0:i386 \
    `# --- networking (both i386 ABIs) ---` \
    libcurl4t64:i386 libcurl3t64-gnutls:i386 \
    `# --- X11 session/toolkit libs games still pull ---` \
    libsm6:i386 libice6:i386 libxt6t64:i386 libxmu6:i386 libxaw7:i386 \
    libxv1:i386 libxxf86dga1:i386 \
    `# --- misc runtime (usb, ltdl, tdb, sound-events) ---` \
    libusb-1.0-0:i386 libusb-0.1-4:i386 libtdb1:i386 libltdl7:i386 \
    libcanberra0t64:i386 libcanberra-gtk3-module:i386 \
    `# --- fonts top-up (CJK + dejavu full) ---` \
    fonts-dejavu-core fonts-noto-core \
  2>&1 | tail -12
  echo "--- ldconfig refresh ---"
  ldconfig
'

echo ">>> [4/4] writing ALSA->PulseAudio bridge (/etc/asound.conf in rootfs)..."
cat > "$ROOT/etc/asound.conf" <<'EOF'
# Route ALSA "default" through PulseAudio so x86 apps that open the raw ALSA
# default device get sound via the host PulseAudio server (see PULSE_SERVER in fexrun).
pcm.!default { type pulse }
ctl.!default { type pulse }
EOF

echo ">>> verifying key artifacts..."
echo -n "  i386 lavapipe ICD: "; ls "$ROOT/usr/lib/i386-linux-gnu/libvulkan_lvp.so" 2>/dev/null && echo OK || echo MISSING
echo -n "  i386 SDL2:         "; ls "$ROOT/usr/lib/i386-linux-gnu/libSDL2-2.0.so.0" 2>/dev/null && echo OK || echo MISSING
echo -n "  i386 OpenAL:       "; ls "$ROOT/usr/lib/i386-linux-gnu/libopenal.so.1" 2>/dev/null && echo OK || echo MISSING
echo -n "  ALSA pulse plugin: "; ls "$ROOT/usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_pulse.so" 2>/dev/null && echo OK || echo MISSING
echo ">>> complete-fex-env.sh DONE."
