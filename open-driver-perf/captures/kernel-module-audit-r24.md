# Kernel module audit against upstream (round 24)

## Base version

`/home/radxa/kernel-src/powervr` is a backport of Linux **6.16** `drivers/gpu/drm/imagination`
(the extracted 6.16 source tree is `/home/radxa/kernel-src/src`). Delta from 6.16 upstream:
pvr_drv.c ~224 diff lines, pvr_fw.c ~131, pvr_context.c ~28, pvr_queue.c ~269 (includes the
round-23 backport and the local chain_frag/combine_pr machinery), pvr_job.c ~83, pvr_device.c ~24,
pvr_ccb.c 0.

## Upstream commits after 6.16 (from git.kernel.org log, checked 2026-10-25) worth noting

| Date | Commit | Relevance |
|---|---|---|
| 2026-03-24 | "Implement handling of context reset notification" (d994acc526c7) | handles FWCCB 105 which this module logs as "unknown FWCCB command" - robustness, not kick |
| 2026-05-19 | "Populate FW common context ID before passing to the FW" (de1e8a590f4e) | per-context FW data; would matter if the ID were used in kick routing |
| 2026-03-23 | "Skip 2nd thread DM association for non META Firmware" (62a6f98cda4e) | device-wide FW thread/DM init |
| 2026-07-31 | "Set scheduler timeout period higher than Firmware timeout" (2224d6642136) | kernel-side timing only |
| 2026-04-08 | job-submission series (8 patches) | already backported (round 23), rest is rename/cleanup |
| 2026-09-24 | "Treat FW connection ctl like other interface structures" (f1db0120f15d), "Don't map FW interface structures unnecessarily" (c4c54ffd6555) | FW interface object mapping |
| 2026-05-19 | "Don't timeout job if its fence has been signaled" (ee7cb7425314) | kernel-side timeout handling |

## Key reasoning

These differences are **device-wide** (established at driver probe / firmware boot / shared FW
init). The open Vulkan driver runs on exactly this module and its geometry kick reaches
`TA finished`, so a device-wide kernel difference cannot by itself explain a per-client stall.
The per-client kernel-visible surface is the UAPI call sequence and its arguments - which is the
one thing never compared between the two clients. That is the next systematic probe (see HANDOFF).

## Not yet present in the module

`grep` confirms the module has none of: CONTEXT_RESET_NOTIFICATION handling, the non-META 2nd-thread
DM association guard, the "don't timeout if fence signaled" check. All are worth backporting for
robustness, but they should not be expected to fix the TA kick on their own.
