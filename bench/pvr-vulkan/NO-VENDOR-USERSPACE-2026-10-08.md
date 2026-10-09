# There is no vendor userspace on this system - the 787 FPS baseline is unreachable

## What the objective assumes

"the vendor reaches 787 FPS through the SAME weston + Xwayland + client + zink where the open stack
gets ~31, so the gap is the Vulkan driver, not the compositor or X11-vs-Wayland."

Running that A/B requires a vendor client stack. **There is none.**

## Evidence

| check | result |
|---|---|
| `find / -name 'libGLESv2.so*'` | only Mesa build trees; no DDK userspace |
| `find / -path '*ddk*' -name '*.so*'` | nothing |
| Vulkan ICDs present | only `pvr_gen_icd.json` / `pvr_test_icd.json`, both -> Mesa `libvulkan_powervr_mesa.so` |
| Mesa pvr on `pvrsrvkm` | `vkEnumeratePhysicalDevices -> -3` (VK_ERROR_INITIALIZATION_FAILED); zink: "failed to choose pdev" |

Mesa does carry a `pvrsrvkm` winsys (`pvr_srv.c`) next to the open `powervr` one, so the intent was
for one Mesa userspace to drive either kernel module - but it cannot initialise against the vendor
module as installed here.

## Consequence

**The vendor baseline cannot be reproduced in this session.** Either it came from a configuration no
longer installed, or it was measured differently than stated. It should not be treated as a
reproducible target without first locating that userspace.

Practical effect on the work: the remaining path is to improve the open stack against its own
measured bottleneck (the ~13 ms explicit-sync release wait), not to close a delta against a vendor
stack that cannot be run.

## Switch safety, observed working

* `switch-vendor.sh` correctly refused while Xwayland was alive (its guard checks Xorg, Xwayland, X
  and kwin).
* Both switches were done only with no X and no weston running, then weston was restarted.
* Restored to the open driver afterwards: weston + Xwayland up, `bda` PASS, `gpu-fw-guard` active.
