# Next actions after the 2026-10-27 reboot (for the next session)

State before reboot: vendor `pvrsrvkm` + desktop, original firmware
(`sha256 1db1c399…`), verify RESULT: OK. PR #12 (TF port) open/mergeable-clean. Open `powervr` stack needs this
reboot because its probe read BVNC 0.0.0.0 after many manual driver swaps.

## 1. Re-verify health (2 min)
`sh tools/verify/verify.sh`, firmware sha, kwin, `readlink` bound driver. Then `switch-safe.sh open` should bind
again; confirm with `readlink` = powervr and a quick `vkrender` PASS
(`PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 VK_ICD_FILENAMES=...`) using the TF-port build ICD.

## 2. Deadline work for PR #12 (10 min)
With the open stack up, run the extra validation for PR #12 and append to it:
- `/tmp/glm2w/usr/bin/glmark2-es2-wayland --validate --size 320x240` under headless weston with the TF build
  (`/tmp/mesa-tf2/build` was a tmp worktree and is gone after reboot; rebuild from
  `mesa-tf-port/*.patch` with `tools/build-ap-mesa.sh`-style steps, or from the exported patch series:
  worktree at `9a2b374` in `~/mesa/mesa-main` + `git am` the 269-series then `mesa-tf-port/*.patch`).
- `vkcube --wsi wayland` for a few seconds under the same weston.
- Restore vendor + desktop afterwards.

## 3. New non-IMG DM2/TA experiment: firmware-build bisect (30-60 min, safe)
IMPORTANT: keep the board recoverable; firmware is just a file, and switch-safe.sh restores vendor on failure.

Rationale: distinguish "the 36.56 build's MTS dispatch path is broken for this client" from
"client/hardware". Boot the same open driver with other official Rogue firmware builds from IMG's linux-firmware
fork (branch `powervr`, https://gitlab.freedesktop.org/imagination/linux-firmware, commit 8a58f818):
`rogue_36.52.104.182_v1.fw`, `rogue_36.50.54.182_v1.fw`, `rogue_36.29.52.182_v1.fw`,
`rogue_36.28.21.182_v1.fw`, `rogue_36.53.104.796_v1.fw` (BVNC mismatch may be rejected by the firmware
compcheck - record which loads and which is refused).

Procedure per candidate:
1. backup: `sudo cp -a /lib/firmware/powervr/rogue_36.56.104.183_v1.fw /tmp/fw-orig.fw`
2. `sudo cp /tmp/fw-<candidate> /lib/firmware/powervr/rogue_36.56.104.183_v1.fw`
3. `switch-safe.sh open` (or rmmod/insmod powervr); `dmesg | tail` → firmware load success or BVNC error.
4. If it boots: run `PVR_PROBE_GEOM_ONLY=1 pvr-probe`; read the firmware trace (mask 2) for `Kick TA` /
   `TA finished` / `GPU has locked up`. Record.
5. Restore `/tmp/fw-orig.fw` and reload before the next candidate.
Final state: original firmware copied back, sha `1db1c399…`, vendor + desktop restored, `verify.sh`.

## 4. Still the only definitive path
IMG observability request: `docs/IMG-OBSERVABILITY-REQUEST-DRAFT.md` (needs the human to send).
If the firmware bisect shows another build dispatches DM2/TA for the native client, that immediately narrows the
bug to a change between the builds (and gives IMG a concrete regression to look at).


## Status 2026-10-27 14:5x (this reboot executed)
- Health OK; open stack back (`switch-safe.sh open` bound powervr, native probe re-confirmed the DM2/TA stall:
  Kick TA -> GPU has locked up).
- **Firmware-build bisect: impossible.** All other official builds are refused with
  "FW built for incorrect GPU ID ... (expected 36.56.104.183)"; the only published 36.56 build is the one we
  run (differs from the 36.52 build by identity bytes only). See TA-KICK-DISASSEMBLY.md round 30.
- Remaining local odds: blind MTS poke sweep (very low), vendor-vs-open static diff (days, medium-low),
  cross-board test (hardware). IMG request = only definitive path.
- PR #12 extra validation (`glmark2 --validate`, `vkcube`) still pending: the TF Mesa build was in /tmp and
  is gone after reboot; rebuild from `mesa-tf-port/*.patch` when needed (the core validation already stands).
- Board restored vendor+desktop, original firmware, RESULT: OK.
