# PR #13: boot into the open stack as the daily desktop, safely

**Goal:** the board boots straight into Plasma on the open stack (open `powervr` kernel driver, open
firmware, this repo's Mesa). The vendor stack stays as an automatic fallback. PR #12 made the open
stack work and showed it is worth it. This PR makes it the default, makes a failed boot recover on
its own, and fixes what still falls short.

**Where PR #12 left it** (`open-driver-perf/tools/desktop-bench/README.md`): at the same 1280x960, the
open stack beats the vendor on Chromium WebGL (44-46 fps vs none), Firefox WebGL (correct picture vs
wrong) and glmark2 (495-526 vs 297). The desktop is GPU-composited, with 0 GPU errors in use. Today it
only runs when switched to by hand: the board still boots the vendor stack.

**Objectives**

1. **Boot into the open stack, and never get stuck.**
   - At boot, load the open GPU driver (`powervr`), not the vendor driver (`pvrsrvkm`), and run
     `open-stack-libs.sh on` before the display manager. Invert `mesa-open-boot-check.service`, which
     today assumes a vendor boot.
   - The display manager starts a Plasma Wayland session. Keep the vendor settings out of it:
     `~/.config/plasma-workspace/env/10-software-render.sh` and the user manager's vendor variables
     (`MESA_LOADER_DRIVER_OVERRIDE=zink` breaks GBM on `card0`). Set `KWIN_DRM_DEVICES=/dev/dri/card0`.
     Use Plasma's normal systemd start, which PR #12's test launcher turned off: new ground.
   - Boot counter: after 2 failed open-stack boots in a row, the next boot uses the vendor stack.
     Reuse the counter in `/usr/local/sbin/bootguard`. A boot counts as good only once the Plasma
     session is up.
   - Make `wedge-guard.sh` a permanent boot service. When it reboots the board, it marks the boot as
     failed, so a GPU wedge cannot loop back into the open stack.
   - Hang detection: the hardware watchdog must fire when PID 1 or logind is stuck in the kernel (that
     state blocks `sudo`), not only when the CPU stops.
   - Fall back to the vendor stack only through a reboot. Never unload a possibly wedged driver while
     the board runs.
   - The open `powervr.ko` is built outside the kernel tree for `6.6.98-5-aw2511`. A kernel update
     would silently drop it: add a rebuild step (or DKMS) and make the fallback catch a missing module.
   - One command switches the default back to vendor.
   - Done when: 5 cold boots in a row land in the open Plasma session. Each of these lands in the
     vendor desktop within 3 reboots, with no power cycle: the driver blocked from loading, kwin
     crashing at start, and a GPU hang forced during boot. A 24-hour open-stack soak passes.
2. **Firefox video at parity.** Firefox drops 29-34 of 598 frames of 1080p video at 1280x960 (vendor
   0, Chromium 1-3), and about half at 1920x1080, scale 1.35. Each frame, the part of Firefox that
   draws the screen waits for the GPU to finish, and it frees GPU memory maps, which is slow. Find
   that per-frame cost in zink, the open Vulkan driver or the kernel, and remove it.
   - Done when: Firefox drops under 1 % at 1280x960 and under 3 % at 1920x1080, scale 1.35.
3. **OpenGL 3.3 and compute shaders.** zink (the layer that runs OpenGL on Vulkan) reports OpenGL 3.2
   and no compute shaders, although the GPU runs compute through Vulkan. Make it report OpenGL 3.3
   with `ARB_compute_shader`.
   - Done when: `glxinfo` shows 3.3 core with `GL_ARB_compute_shader`, and a GL compute test in
     `tools/verify` gives correct results. (Wine's D3D11 compute still needs OpenGL 4; that path is
     DXVK, below.)
4. **Firmware resets when a client is killed.** Killing a GPU client mid-frame makes the firmware
   report a context reset (`FWCCB 2abc0069`). Closing an app is normal on a desktop. Show that other
   apps and the desktop keep running through it, or fix it.
   - Done when: killing glmark2, Firefox and Chromium mid-frame, 20 times each, leaves the desktop
     drawing correctly with no job timeouts.
5. **Repo docs say what is true now.** `README.md` "Honest summary" still says the GPU "cannot drive a
   GPU-composited desktop". `STATUS.md`, `PERF-PARITY-STATUS.md`, `GLES-PARITY-ROADMAP.md` (standing
   rules, latest patch number) and `system/open-stack/README.md` (assumes a vendor boot) are stale.

**Out of scope, separate PRs:** DXVK-Sarek v1.12.0 crashing after `vkCreateDevice` with Hangover 11.16
(same on both stacks, so not the GPU stack); full OpenGL 4.x (needs tessellation shaders, which this
GPU's drivers do not offer); GLES 3.x in the native `pvr` driver. Not the GPU stack: box64's `libXt`
symbol failure, and x86 Chrome missing from the FEX rootfs.

**Final check before merge:** the boot tests (objective 1), the full desktop benchmark at 1280x960 and
at 1920x1080, the daily-app matrix and `tools/verify/run-all.sh`. Every result is checked by screenshot
(`vcheck.py`): a wrong picture fails the run, even with good numbers. 0 GPU errors while in use.
