# Session summary - 2026-10-27

## Goal status
Native open PowerVR GL frontend (`pvr_native`) is **still blocked**: the driver initialises, compiles, records
and submits real streams, and its compute-fallback path renders, but the direct geometry kick never receives
the DM2 completion event (firmware times out, HWRs). **No local lever remains**; the only definitive path is
IMG-side firmware/MTS observability. GL itself works through zink -> open Vulkan.

## Delivered today
- **v0.1.1 release** (PR #11 merged as `0eb8099`): open-driver Mesa series 44-269 (native `pvr` driver work +
  probe tooling) and the `open-driver-perf/` evidence tree; firmware provenance corrected (the board's open
  firmware is the official IMG 36.56 build, sha `1db1c399…`, from IMG's linux-firmware fork branch `powervr`).
- **PR #12** (`mesa-tf-port-2026-10-27`, open, mergeable-clean): official Mesa MR !44385
  `VK_EXT_transform_feedback` series + 2 u_trace prerequisites + 3 integration fixes = patches 0270-0294;
  whole series now **294 patches** (173 files, +25495/-928), `git am` reproduces the ported tree exactly.
  Result: **zink GLES 3.1 / GL 3.2-class** (was ES 2.0 / GL 2.1), Vulkan PASS as user and root, glmark2
  **578-608** vs 543-594 before (no regression).
- **v0.1.2 release notes drafted** (`docs/RELEASE-NOTES-v0.1.2-DRAFT.md`) - publish after PR #12 merges.

## Ruled out for the DM2/TA stall (all documented in TA-KICK-DISASSEMBLY.md rounds 1-32)
firmware image and identity/BRN variants; firmware version/build (all other official builds are refused by
the kernel: "FW built for incorrect GPU ID"); driver version (upstream + 30 backported fixes, apercel);
context/stream/HWRT/RTData/FWCtx data; kernel KCCB KICK command (byte-identical modulo addresses); job
flags/priority/sync shapes; warm GPU; MTS host pokes (MTS ignores them); vendor-vs-open firmware kick-path
code (structurally identical); user vs root privileges.

## Open actions (need the human)
1. **Merge PR #12** -> publish v0.1.2 with the drafted notes.
2. **Send the IMG observability request** (`docs/IMG-OBSERVABILITY-REQUEST-DRAFT.md`) - the only definitive
   route for the DM2/TA stall.

## Board / repo state
vendor `pvrsrvkm` + desktop, original firmware (`/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`,
sha256 `1db1c399…`), `tools/verify/verify.sh` -> RESULT: OK, 0 faults. Repos clean:
`~/a733-powervr-fex` (public, trixie = `0eb8099`), `~/Desktop/Projects/Radxa-A7A` (evidence), Mesa branch
`claude/perf-2026-10-10` (TF port branch `mesa-tf-port-2026-10-27` pushed).

## Resuming
- Board/driver: `open-driver-perf/tools/verify/switch-safe.sh open|vendor` (auto-rollback; never load vendor
  while X/kwin holds the GPU); after many manual swaps the open probe can read BVNC 0.0.0.0 -> reboot.
- TF Mesa: rebuild from `mesa/open-driver-series/*.patch` or `mesa-tf-port/*.patch`; run with
  `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` and the dev-env ICD.
- Evidence/benchmarks: `captures/`, `RESULTS.md`, `STATUS.md`, `PRIOR-ART.md`, `NEXT-AFTER-REBOOT.md`.
