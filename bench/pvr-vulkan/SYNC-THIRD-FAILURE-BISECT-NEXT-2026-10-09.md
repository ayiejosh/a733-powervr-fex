# THIRD failure: the two-line design also breaks weston — the self-wait was NOT the cause

## What was tried

**The two-line design implemented exactly as specified** — line A (`job_sync`/`job_value`) and line B
(`job_sync_b`/`job_value_b`), **alternated by counter parity so a barrier never waits the object it signals**,
with `last_job_signal_sync` untouched and the barrier's wait on the previous barrier's signal preserved.
**Built cleanly (0 errors).**

## The result

| test | result |
|---|---|
| `bda` | **PASS (0 failures)** |
| `vk13` | **PASS (11 ok, 0 failed)** |
| `pctest` | **PASS (0 failures)** |
| `vk16` | **PASS (9 ok, 0 failed)** |
| `vkrender` 512 / 2048 | **PASS** |
| **weston** | **SIGSEGV** |

**Identical to attempts 1 and 2: every probe green, weston crashes.** Reverted; desktop restored.

## What this falsifies

**My design analysis was wrong.** I concluded the failure was "a barrier waiting on and signalling the same
timeline object in one submit" — and the two-line form makes that **impossible by construction** — **yet the
crash is identical. So the self-wait was not the cause.**

**Three hypotheses now falsified:**

| # | hypothesis | fix applied | outcome |
|---|---|---|---|
| 1 | event paths lost their wait on `last_job_signal_sync` | restored it | **still crashed** |
| 2 | barrier lost its wait on the previous barrier's signal | restored it | **still crashed** |
| 3 | self-wait on one timeline object | removed structurally | **still crashed** |

## What is actually known

- **Every probe passes; weston crashes** → the fault is in a path the probes never exercise
- **The crash is immediate and total** (`Connection reset by peer`) → consistent with the **first submit** being
  malformed, or the queue being torn down incorrectly
- **The full map has ~44 references** to those two fields — including `pvr_queue_finish` teardown, the null-job
  path and two event paths — **and only four were converted**

## The right next approach — bisection, not another hypothesis

**Stop guessing at the mechanism.**

1. **Convert GEOM's barrier only, leaving the GEOM job's wait on the OLD `next_job_wait_sync`.** If weston
   survives, the fault is in the **job-wait** side; if it crashes, it's in the **barrier** side.
2. **Or instrument**: make the converted path *also* signal a per-job sync the old wait reads, so ordering is
   preserved by **both** mechanisms — then the crash tells you whether it's ordering or lifetime.

**Gate unchanged: probes are blind, weston is the only detector.**
