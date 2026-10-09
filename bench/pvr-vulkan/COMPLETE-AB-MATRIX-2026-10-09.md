# The complete both-driver matrix — one instrument, one run

First full A/B this session from a single instrument in a single run. `ab.sh` closes the desktop, runs arm
**open** fully, switches, runs arm **vendor** fully, reopens the desktop via a **trap**, prints the diff.

| probe | open | vendor | ratio |
|---|---|---|---|
| **`cstp` (integer, no loop)** | **361.0 M inv/s** | **369.8** | **1.02× — PARITY** |
| `cstpf` (float loop) | 88.2 | 145.8 | 1.65× |
| `cstpi` (integer loop) | 70.7 | 144.6 | 2.05× |
| `cstpin` (register-only loop) | 72.0 | 154.2 | 2.14× |
| `vkheavy` 2048 | 255.938 ms | 180.153 ms | 1.42× |
| `vkrender` 512 | 1.507 ms | 0.682 ms | 2.21× |
| `vkrender` 1024 | 3.979 ms | 1.707 ms | 2.33× |
| `vkrender` 2048 | 13.746 ms | 6.488 ms | 2.12× |
| `vkrender` 4096 | 53.892 ms | 27.796 ms | 1.94× |

## What this settles that hand-measurement could not

**1. `cstp` — straight-line compute — is at PARITY (1.02×).** The open driver is **not universally slower**;
the deficit is specific to **loops** and **rendering**. Any "the open driver is N× slower" claim without a
named workload is wrong.

**2. The render gap is ~2× and FLAT from 512 to 4096** (2.21 / 2.33 / 2.12 / 1.94) — the same conclusion as
the separate size sweep, now from one consistent instrument in one run.

**3. `cstpin` (2.14×) is now the WORST compute case — worse than `cstpi` (2.05×).** `cstpin` was the
register-only control built to isolate immediate materialization. **Its being worst is consistent with the
final mechanism finding: the cost is register placement, not immediates.** The probe that was meant to be the
clean control has become **the clearest evidence for the conclusion.**

## Consistency check

Every open value matches earlier hand measurements (`cstpi` 70.7 vs 72.7/72.8; `cstpf` 88.2 vs 87.9/87.0;
`vkheavy` 255.9 vs 255.8; `vkrender` 2048 13.746 vs 13.706). **The harness, built later, reproduces the hand
measurements to within noise** — the check that it measures the same thing.

## A caveat about the diff

`vkrender` 256 and `cstpi1` show only a vendor row: **stale records from earlier vendor-only runs**, not
missing open data. The diff merges the whole log, so a probe absent from the current run still appears if it
was ever measured. **A diff that silently mixes runs is misleading** — the driver field and record order are
what let that be spotted.
