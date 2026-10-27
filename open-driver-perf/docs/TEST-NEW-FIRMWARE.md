# Runbook: test a new open-ABI firmware for BXM-4-64 BVNC 36.56.104.183

When a genuine `rogue_36.56.104.183_v1.fw` (or an updated BXM image) appears, this is the whole procedure;
it takes ~10 minutes and leaves the board on the vendor stack.

## 0. Preconditions
- Board on the vendor stack with the desktop up (`readlink /sys/bus/platform/devices/1800000.gpu/driver`
  -> `pvrsrvkm`), no GPU clients.
- Keep the original image safe: it is at `/home/radxa/gpu-fw-backup/rogue_36.56.104.183_v1.fw.orig`
  (sha256 `1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`).

## 1. Get the image
`tools/firmware/check-open-firmware.sh` prints the linux-firmware URL if the file appears. Save it to /tmp and
record `sha256sum`. Check the FW info header (last 4 KiB, `info_version` must be 3, `flags` must have the
open-source bit): the file must be an **open-ABI** build for BVNC 36.56.104.183, not the vendor closed one.

## 2. Switch to the open stack and install it (temporarily)
```
sudo systemctl stop display-manager; sleep 4
# make sure no kwin/weston/Xwayland/Xorg holds /dev/dri before switching
/home/radxa/open-perf/open-enter.sh
sudo cp /tmp/rogue_36.56.104.183_v1.fw /lib/firmware/powervr/
sudo rmmod powervr; sudo insmod /lib/modules/$(uname -r)/extra/powervr/powervr.ko
sudo dmesg | tail -5          # expect "loaded firmware ..." and "FW version vX.Y (build N OS)"
```

## 3. Run the native geometry probe and read the firmware trace
```
sudo sh -c "echo 2 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask"
cd /home/radxa/mesa/mesa-main
PVR_PROBE_GEOM_ONLY=1 timeout 60 ./build/src/gallium/drivers/pvr_native/pvr-probe | tail -5
sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0 | grep -E "Kick TA|TA finished|locked up"
```
Success = the line "geometry-only (draw stream) COMPLETED" **and** `TA finished` after the native client's
`Kick TA` line. Today (relabelled 36.52 image) we get `Kick TA` then `GPU has locked up` ~125 ms later.
Also run the full probe (`./pvr-probe | tail -3`, expect `--- ALL PASS`) and the benchmark
(`PVR_PROBE_COMPUTE_BENCH=1 ./pvr-probe`, expect ~1.4 ms/frame at 512x512).

## 4. If the TA runs: the next steps
- The native driver's fixed-function path is no longer blocked; the next work is wiring the real GL frontend
  (state tracker) and then the glmark2 benchmark the goal asks for. The compute-fallback path stays as a
  correctness reference.
- Record the firmware sha256 + FW build number in RESULTS.md / HANDOFF.md.

## 5. Always restore (even if the test fails or the GPU locks up)
```
sudo cp /home/radxa/gpu-fw-backup/rogue_36.56.104.183_v1.fw.orig /lib/firmware/powervr/rogue_36.56.104.183_v1.fw
sudo rmmod powervr
/home/radxa/open-perf/open-exit.sh
# wait for kwin, then verify: sha256 matches, `tools/verify/verify.sh` -> RESULT: OK
```
If the GPU locks up, the kernel driver recovers on rmmod/insmod; if the board wedges, reboot and re-run the
restore step above.

## Safe restore after firmware experiments (2026-10-27 incident)

A mid-experiment restore once crashed the board: loading the vendor `pvrsrvkm` while X/sddm still held GPU
connections triggers the known `PVR_K: psConnectionPriv is NULL in LinuxServicesConnectionFromFile()` NULL
dereference (Oops 96000004). The order matters:

```
sudo systemctl stop display-manager
sleep 5
pgrep -x X; pgrep -x kwin_x11        # must both be empty; if not, kill and wait again
sudo rmmod powervr 2>/dev/null
sudo modprobe pvrsrvkm               # only now, with no userspace GPU clients
sleep 2
sudo systemctl start display-manager
```
After any experiment that **patched the firmware or tampered with the GPU** , prefer a clean reboot over a
hot module swap - the reboot path is reliable, the hot swap has this crash window.

## Recovering the open driver after repeated driver swaps (2026-10-27)

Repeated manual `unbind`/`bind`/`rmmod`/`insmod` cycles can leave the GPU clock/power state such that the
open `powervr` probe reads **BVNC 0.0.0.0** and fails with
`Direct firmware load for powervr/rogue_0.0.0.0_v1.fw failed` - the module loads but never binds, and Vulkan
then fails enumeration. Recovery without a reboot (now built into `tools/verify/switch-safe.sh open`):
load the vendor module once (`modprobe pvrsrvkm`, which re-initialises the clocks through its platform init),
remove it, and load `powervr` again. If that still fails, a clean reboot is the reliable fallback.

`tools/verify/switch-safe.sh open` now: removes any stale unbound `powervr` module first, retries through a
vendor probe cycle with the GPU's `power/control` reset to `auto`, and - if the open driver still will not
bind - restores the vendor stack and desktop before aborting, so a failed experiment never leaves the board
headless. The observed failure after many manual swaps was `rogue_0.0.0.0` (BVNC read as zero); only a clean
reboot restored the open stack.
