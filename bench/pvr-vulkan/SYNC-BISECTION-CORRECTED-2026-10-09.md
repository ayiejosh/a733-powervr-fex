# The bisection I proposed is FLAWED — the halves are coupled. Here is the one that works.

## Why "convert the barrier only" is not a bisection

**The barrier sets what the next job waits on; the job waits on what the barrier set** — two ends of one
mechanism.

- **Barrier-half only:** the barrier no longer *sets* `next_job_wait_sync`, so **the job waits NULL** —
  ordering silently lost. **Not a bisection, a third broken variant.**
- **Job-wait-half only:** the job waits a timeline point **the barrier never signals** — same problem.

**A half-conversion always loses ordering, so "barrier vs job-wait" cannot distinguish anything.**

## The axis that works

**Keep BOTH mechanisms live and see which one the crash follows:**

- **Variant X:** barrier creates **and** signals the per-job sync (**old**), **and** the job waits a timeline
  point the barrier **also** signals via a **second** `null_job_submit`. **Both orderings present.** Survives →
  **the fault is in removing the per-job path, not in the timeline.**
- **Variant Y:** the reverse emphasis.

**The question: does *adding* timeline syncs break it, or *removing* per-job syncs? Neither loses ordering, so
either outcome is informative.**

## The cheaper diagnostic that splits the problem in two

**Run weston under the converted build with the queue teardown made non-freeing (leak the syncs):**

| outcome | conclusion |
|---|---|
| **crash disappears** | **lifetime** bug (double free / use-after-free), not ordering |
| **crash persists** | **ordering or submit shape** — the per-job structure is the difference |

**One test, two very different investigations — run it BEFORE any variant X/Y work.** It's a two-line change
and it eliminates half the space.

## The meta-lesson

**Three attempts, three falsified hypotheses, all from reasoning about the mechanism.** The corrected plan
**does not depend on being right about the mechanism**: leak-to-test splits **lifetime** from **ordering**;
variants X/Y split **adding** from **removing**. **That is the shape of plan to use when three designs have
failed.**
