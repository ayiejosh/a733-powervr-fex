# pvr-vulkan bench — PowerVR open stack vs vendor (Radxa Cubie A7A)

## Start here

1. **`FINAL-HANDOVER-2026-10-08.md`** — the full record. **Read its top banner first:** sections 1–20 carry the
   original framing, **21–24 are the corrected record**, and where they disagree the later section wins.
2. **`HARNESS-LOG-SUMMARY.md`** — every measurement recorded, with observed ranges.

## The tools

| tool | what it does |
|---|---|
| `harness.py <probe> <size> <count> [ENV=V …] [--driver=open\|vendor]` | **one run → driver, speed (min/median/max + spread), correctness, per-stage job durations, critical path, CPU split, bandwidth.** Appends to `harness-log.jsonl`. **Its `--driver=` checks for kwin and refuses to switch if a desktop is live.** |
| `sweep.sh` | the whole probe matrix → one consolidated table |
| `ab.sh` | **full A/B in one run** — closes the desktop, arm open fully, arm vendor fully, reopens via trap, prints the diff |
| `components.sh` | 30 component checks — modules, vermagic, driver binding, firmware (+md5), ICDs **and whether each named library resolves**, tracepoints, guard |
| `enumvk.c` | Vulkan extension/feature enumerator |

## The probes

`vkrender`, `vkheavy` (render) · `cstp` (no loop), `cstpi`/`cstpf`/`cstpin`/`cstpi1` (loops) ·
`cstpi128`/`cstpi512` (long loops) — **all built from the same harness**.

## Headline findings

* **~2.4x render gap** (2.39x median, **1.94x worst case**) — measured with ranges on both sides.
* **The four PCO fixes are PROBE-LEVEL: 2.7–3.4x on loop-bound compute and NO measurable client effect** on two
  scenes. **Do not describe them as a client-level win.**
* **Kernel share of the client frame: 62.5%**, independently measured.
* **Stage shapes differ:** geometry is **flat** (open wins, 0.43x); PR and fragment are **tile-bound**, so the
  PR's 4.03x is a *work-shape* problem (Mesa's) and the fragment's 2.17x is a *raster-cost* problem (outside Mesa).

## Safety — read before switching drivers

**The vendor `pvrsrvkm` driver has crashed this board twice**: its rewrapped firmware faulting, and a NULL-deref
in its **file-close path** during a driver-switch sequence. **Both required a reboot; the guard recovered
automatically both times.**

* **Never unbind while kwin/X is alive** — the guard and `harness.py` both refuse.
* **Batch switch operations.** Repeated weston teardown under the vendor driver is the exposure.
* **Prefer `./ab.sh`** for two-arm work: one controlled switch pair with a trap-restore.

## Measurement discipline

* **throughput / kernel timestamps / counts** — ~1% repeatable, insensitive to host load. **Use these.**
* **per-stage job durations** — 0.7–4.4% repeatable. **The decomposition's foundation.**
* **wall-clock FPS** — ~0.2 ms fixed jitter, so percentage spread scales inversely with frame time
  (**4.5% at 2048, 27% at 512**). **Never a 3-run median at small sizes.**
* **Always check the tool measured what it claims** — two of this session's errors were instrumentation
  silently not measuring (a mislabelled driver, a missing client process).
