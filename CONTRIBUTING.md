# Contributing

This repo is meant to be a **living, reproducible baseline** for the Allwinner A733
PowerVR + FEX stack. Test it, benchmark it, push the walls, fix bugs — and send the
findings back so the next person starts further ahead.

## Ways to contribute (all welcome)

- **Run the benchmarks** ([`bench/`](bench/)) on your board and add your numbers to
  [`RESULTS.md`](RESULTS.md) — cross-board data is the whole point. Even "same board,
  different kernel/DDK" rows are valuable.
- **Reproduce or challenge a finding** in [`docs/FINDINGS.md`](docs/FINDINGS.md). If
  something that's marked a "wall" works for you (newer DDK, mainline, a flag we
  missed) — that's a great PR/issue. Findings are dated observations, not gospel.
- **Optimize**: faster FEX (AOT/code cache), better Zink coverage, a working
  Wayland-client path, GPU desktop improvements, encode work.
- **Fix bugs** in the scripts/patch, or port the kernel patch to a new BSP/kernel.
- **Extend**: new device-tree overlays, other A733 boards (A7S/A7Z), mainline tracking.

## Benchmark discipline (required for perf changes)
Any change that could affect performance **must carry a before→after `bench/run.sh`
diff in its commit message**, and `bench/baseline.txt` is updated only for verified,
intended improvements. Compare the `ratio.*` + `gpu.*` lines (reliable); raw absolutes
are ±~20% load-noise on an always-on box. See [`bench/WORKFLOW.md`](bench/WORKFLOW.md).

## How to submit
1. Fork → branch → PR. Keep PRs focused; describe what you tested and on what
   (board, kernel `uname -r`, DDK `strings /usr/lib/libVK_IMG.so* | grep -m1 24.`).
2. For results, edit [`RESULTS.md`](RESULTS.md) or open a **Benchmark result** issue.
3. For "works/doesn't on my board", open a **Board test report** issue.

## Ground rules
- **No proprietary blobs in commits** — never commit the PowerVR userspace/firmware,
  the FEX rootfs, or third-party app binaries. The `.gitignore` blocks the obvious
  ones; the install/recipe model fetches those from the vendor (see README).
- **No secrets** — keys, tokens, host identifiers.
- Patches against vendor sources should be **diffs**, applied on top of the vendor's
  own distribution (don't paste their source in).
- Be honest about methodology: single-board numbers are indicative; say how you measured.

## Open problems / help wanted
These are the live walls from `docs/FINDINGS.md` — concrete places to dig:
- **Wayland GPU *clients*** crash (zink `kopper` SIGSEGV on the img Vulkan ICD). The
  compositor GPU-composites; client apps presenting via `wl_egl_window` don't.
- **Transparent EGL→GPU** (no PowerVR GLVND vendor; vendor EGL is X11-only fork).
- **FEX cold-start** is JIT-compile-bound — an AOT/persistent code cache is the lever.
- **Steam under FEX** — CEF `steamwebhelper` bwrap/pressure-vessel blocker.
- **Mainline tracking** — `drm/imagination` + Mesa `pvr` for A733 (`sun60iw2`); the
  upstream DTS effort is the path that lifts the hard walls. Test/track it.
- **HEVC hardware *encode*** — vendor-lib limit (no IDR); H.264 encode works.
