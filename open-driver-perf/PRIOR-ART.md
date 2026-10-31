# Prior-art ledger — PowerVR BXM-4-64 / A733 open stack

Rule for every session: **search before building.** If someone has done it, replicate and continue from
the furthest working state; only start independent exploration when nothing exists (or the prior art
provably does not apply), and always record sources, what was tried, and what was ruled dead below.
Record negative results too — a dead end that costs a day once must never cost a day again.

## Sources found and what they gave us

| Source | What it provides | State / what we did with it |
|---|---|---|
| [IMG linux-firmware fork, branch `powervr`](https://gitlab.freedesktop.org/imagination/linux-firmware) | Official open-ABI firmware images (15 Rogue builds incl. `rogue_36.56.104.183_v1.fw`) | **Adopted**: board firmware sha256 `1db1c399…` is byte-identical to IMG's `8a58f818` build — no firmware request to IMG needed for the image. |
| [XDC 2026 "PowerVR: A Year of Progress"](https://indico.freedesktop.org/event/12/contributions/528/attachments/378/542/XDC%202026%20PowerVR%20A%20Year%20of%20Progress.pdf) | IMG's roadmap: Vulkan + **zink for GL**; GLES 3.1 via transform feedback; firmware updates planned; firmware emulator/simulator internal | Read 2026-10-27. Confirms IMG does not pursue a native Gallium GL driver; firmware update effort supports our watch strategy; no public debug firmware/emulator. |
| [apercel](https://github.com/TumRedSun/apercel), fork [CheezeCS/apercel](https://github.com/CheezeCS/apercel) | Out-of-tree upstream drm/imagination + 30 backported fixes; Mesa patch (transform feedback / GLES 3.1) | Built driver for this board (firmware `v1.1 build 6976702 OS`, clock 1008 MHz): **native TA stall unchanged**. Built Mesa patch: Vulkan PASS, GLES 3.1 via zink works (glmark2 166–175 on vanilla, 3.1× slower than our perf branch → port to perf branch is the queued work). |
| [CheezeCS/armbian-a733-gpu](https://github.com/CheezeCS/armbian-a733-gpu) | A733 integration: apercel build, Mesa packaging, firmware URL, HDMI/fan fixes | Read; source of the IMG firmware URL + apercel pin. |
| [Mesa MR !44385](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44385) | `VK_EXT_transform_feedback` → GLES 3.1 through zink (IMG: "last piece of the puzzle", targeting Mesa 26.3) | This is what the apercel Mesa patch ports; our perf branch predates it (GL 2.1/ES 2.0). Port queued (`docs/APERCEL-EXPERIMENT.md`). |
| [mesa-pvr-vf2](https://github.com/cwt-vf2/mesa-pvr-vf2) | "Add PVR Gallium driver" by Brendan King (IMG) | Read: a Gallium **frontend to IMG's proprietary userspace**, not an open-kernel 3D reference → not applicable to the native driver. |
| kernel `drm/imagination: Disable DM overlap for devices with tiny SLC` (6c82d101) | HW workaround for DM-overlap lockups on tiny-SLC parts | **Dead end**: the fix is already present in our kernel backport (and apercel); stall is unrelated. |
| [linux-firmware mainline](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) | Public firmware images | 36.52-era only for BXM; superseded by IMG's own fork. Watch script now tracks the IMG branch. |

## Dead ends already paid for (do not redo)

All in `TA-KICK-DISASSEMBLY.md` with evidence; in short: firmware identity/BVNC byte and BRN-mask variants;
forced alternate kick path; context priority LOW/HIGH; HWRT dataset index; extra first contexts; sync-op
shapes incl. no-sync; job flags FIRST/LAST/none; fragment-only real stream; partial-render jobs;
preceding working Vulkan kick and GPU warm-up; MTS_SCHEDULE register pokes; kick-path branch instrumentation
(both clients take the same branch); BO cache flags (probe already matches the Vulkan winsys); kernel
DM-overlap fix (already present); official-firmware swap; the newest driver with 30 upstream fixes.

## Independent-exploration queue (nothing else has this)

1. **IMG-side observability** (only definitive path): ask IMG why MTS never dispatches DM2 for a non-Mesa
   client given byte-identical kick inputs, and/or for firmware-side logging/debug build. Request doc is
   ready; filing is a human action.
2. **Newer official firmware**: watch IMG's `powervr` branch (script does this); retest any new 36.56 build.
3. **MTS full-block µs differential** (done 2026-10-27, see below): no control-register candidate found.
4. **Host poke sweep during the 29 ms pending window** at registers that only the working kick touches.
   Blind, low odds, GPU may wedge (recoverable by reboot) — only if 3 turns something up.
5. **Vendor-vs-open 36.56 firmware static diff** (both ELFs, different codegen — function-matching project,
   days; medium-low odds).
6. **Cross-board test** (another A733 board, e.g. Orange Pi 4 Pro with apercel) to separate client from
   board init — needs hardware.

## 2026-10-27 session result (MTS full-block differential)

Sampled `0xb00–0xd00` (whole MTS window, 141 regs, µs rate) during a native kick and 40 Vulkan geometry
kicks. Vulkan-only activity: `0x130` (EVENT_STATUS, working completions), `0xb98`, `0xbb0`, `0xc50`,
`0xc68` (1→0 pulse, the kick-handshake poll bit), `0xcc8` (incrementing dispatch counter). Native reaches
none of them. All are **consequences** of a dispatched DM2/TA (event/status/counters), not control
registers, so there is no host-poke target — lever 3 closed.
