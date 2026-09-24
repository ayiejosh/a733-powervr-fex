#!/bin/bash
# Run any command against the open (mainline powervr + Mesa pvr) stack, safely.
#
# Swaps the GPU module, runs the command, restores the desktop - the same shape as
# the benchmark script but with no display phase at all, so it is safe to use for
# pure compute/memory probes. Restore is armed on a timer so a failure here cannot
# leave the desktop down.
#
#   ./open-run.sh <command> [args...]
set -u

# rmmod/modprobe/insmod live in /usr/sbin, which a non-login shell does not put on
# PATH. Without this the script used to get all the way to taking the desktop down
# and only then fail with "rmmod: command not found" - the worst possible moment
# to find out.
export PATH="/usr/sbin:/sbin:$PATH"

# The X server and sddm run as root/the sddm user, so the session teardown and the
# module swap need privileges. Passwordless sudo is available on this board.
SUDO="sudo -n"

SPIKE=/home/radxa/kspike
LOG=$SPIKE/open-run-$(date +%Y%m%d-%H%M%S).log
# The ICD manifest is a generated file inside the build tree, and it has been
# observed to disappear (ENOENT for every process in the swap window, present
# again afterwards with its mtime unchanged). While it is missing the loader logs
# "Found no drivers" and every test fails with VkResult -9 from vkCreateInstance,
# which looks exactly like a broken driver. Prefer the copy kept outside the build
# tree so a run cannot be invalidated that way.
MESA_ICD=${MESA_ICD:-}
if [ -z "$MESA_ICD" ]; then
    for _c in /home/radxa/kspike/open-icd.json \
              /home/radxa/mesa/mesa-main/build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json; do
        [ -f "$_c" ] && { MESA_ICD=$_c; break; }
    done
fi
[ -n "$MESA_ICD" ] || { echo "no Mesa pvr ICD manifest found - regenerate it with ninja"; exit 2; }

say() { echo "[open-run $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

refs() { awk '$1=="pvrsrvkm"{print $3}' /proc/modules; }

restore() {
    say "--- RESTORE ---"
    $SUDO rmmod powervr 2>/dev/null
    $SUDO rmmod drm_gpuvm 2>/dev/null
    sleep 1
    $SUDO modprobe pvrsrvkm
    for _ in $(seq 1 15); do sleep 2; [ -e /dev/dri/card1 ] && break; done
    if [ ! -e /dev/dri/card1 ]; then
        say "card1 missing - reloading the vendor module cleanly"
        $SUDO rmmod pvrsrvkm 2>/dev/null; sleep 2; $SUDO modprobe pvrsrvkm
        for _ in $(seq 1 15); do sleep 2; [ -e /dev/dri/card1 ] && break; done
    fi
    # A stop or start job can outlive its D-Bus call ("Connection timed out" while
    # the unit is still active), so never block here. Restart (not start): after a
    # teardown sddm can be sitting on a bare greeter, and `start` on an already
    # active unit would leave the user with no session at all.
    timeout 30 systemctl reset-failed display-manager 2>/dev/null
    timeout 60 $SUDO systemctl restart display-manager 2>/dev/null
    for _ in $(seq 1 30); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    if ! pgrep -x kwin_x11 >/dev/null; then
        timeout 60 $SUDO systemctl stop display-manager 2>/dev/null
        $SUDO pkill -9 -x X 2>/dev/null
        sleep 2
        timeout 60 $SUDO systemctl start display-manager 2>/dev/null
        for _ in $(seq 1 20); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    fi
    say "desktop: X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell)"
    say "evidence: $LOG"
}
trap restore EXIT

say "log: $LOG"
say "command: $*"
say "pre-state: pvrsrvkm refs=$(refs) X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11)"

# Stop the desktop in a way that cannot hang and cannot leave a handle behind.
#
# Every reference to the vendor module corresponds to an open render-node handle,
# and those are held by root-owned processes (X, sddm, kmscon) as well as by the
# user's KDE session (kwin, kded6, ksmserver, ...). `pkill -u sddm` is deliberate:
# killing every `systemd --user` would take this harness's own service down with it.
DESKTOP_PROCS="X kwin_x11 plasmashell picom kded6 ksmserver kaccess kdeconnectd
               kmscon DiscoverNotifier xdg-desktop-portal xdg-desktop-portal-kde"

if systemctl is-active --quiet kmsconvt@tty1; then timeout 30 $SUDO systemctl stop kmsconvt@tty1 2>/dev/null; fi
timeout 30 $SUDO systemctl stop display-manager 2>/dev/null
for _p in $DESKTOP_PROCS; do pkill -x "$_p" 2>/dev/null; done
$SUDO pkill -9 -x sddm 2>/dev/null
$SUDO pkill -9 -u sddm 2>/dev/null
$SUDO pkill -9 -x X 2>/dev/null
$SUDO pkill -9 -x kmscon 2>/dev/null
for _p in $DESKTOP_PROCS; do pkill -9 -x "$_p" 2>/dev/null; done

for _ in $(seq 1 30); do
    [ "$(refs)" = "0" ] && break
    sleep 1
done
say "pre-rmmod: pvrsrvkm refs=$(refs) X=$(pgrep -c -x X) sddm=$(pgrep -c sddm)"

$SUDO rmmod pvrsrvkm || { say "ABORT: rmmod failed (refs=$(refs))"; exit 3; }
for m in drm_exec gpu-sched drm_shmem_helper; do $SUDO modprobe "$m" || say "WARNING: modprobe $m failed"; done
$SUDO insmod "$SPIKE/mod/drm_gpuvm.ko" 2>/dev/null
$SUDO insmod "$SPIKE/img/powervr.ko" 2>/dev/null
sleep 3
say "open driver: powervr=$(grep -c '^powervr ' /proc/modules) dri=$(ls /dev/dri | tr '\n' ' ')"

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
if [ "${1:-}" = "--gl" ]; then
    size=${2:-512}; frames=${3:-20}
    say "--- zink GL test over the open driver (${size}x${size}, $frames frames) ---"
    ( cd /home/radxa/trixie-prep/bench/pvr-vulkan && runuser -u radxa -- env HOME=/home/radxa \
        LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
        GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
        MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
        DRM_RENDER_NODE=/dev/dri/renderD128 MESA_GLES_VERSION_OVERRIDE=3.2 \
        PVR_TIMING=1 \
        VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 ./glheadless "$size" "$frames" 2>&1 \
        | grep -E 'GL_RENDERER|GL_VERSION|timing ms/frame|frame\(s\)|RESULT|VERDICT|FAIL' \
        | sed 's/^/    /' | tee -a "$LOG" )
    say "command exit: ${PIPESTATUS[0]}"
    exit 0
fi

say "--- command output ($MESA_ICD) ---"
VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 "$@" 2>&1 | sed 's/^/    /' | tee -a "$LOG"
say "command exit: ${PIPESTATUS[0]}"
