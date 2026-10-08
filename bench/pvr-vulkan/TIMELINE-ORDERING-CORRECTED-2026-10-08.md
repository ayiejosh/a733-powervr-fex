# The slot enumeration shows the plan's ORDERING is wrong, not just its isolation assumption

Complete set of users of `next_job_wait_sync[]` / `last_job_signal_sync[]` — **both arrays are used only in
`pvr_arch_queue.c`**, nothing outside touches them:

| site | role |
|---|---|
| 233–240 | `pvr_queue_finish`: destroy both arrays |
| 264–275 | `pvr_update_job_syncs`: destroy old wait + signal, store the new signal |
| 335, 363 | render: wait GEOM / wait GEOM+FRAG |
| 434, 464, 500 | compute / transfer / query waits |
| 529, 531 | read `last_job_signal_sync[stage]` |
| **561–581** | **reads `next_job_wait_sync[stage]`, destroys it, then sets it to a new signal** |
| 606, 610 | reads `last_job_signal_sync[stage]` |
| **717–753** | **same read/destroy/set pattern for `next_job_wait_sync[stage]`** |
| 932–942 | reads both for a wait |
| 959–966 | destroys both |
| 994–998 | reads `last_job_signal_sync[i]` |
| 1050–1055 | reads `next_job_wait_sync[i]` |

## The finding that matters

**Lines 561–581 and 717–753 don't merely read those slots — they OWN them**: they destroy the existing sync
and install a new one. **The event/barrier paths write the same array slots the render path uses.**

**Consequence: the plan's ordering ("convert the render path first, event paths last") cannot work.**
Removing the render path's `pvr_update_job_syncs()` does not retire the slot — the event paths still manage
it, and nothing now maintains the invariant they relied on. **That is consistent with the observed
`vkrender` SIGSEGV**, though reading alone cannot pin the exact faulting dereference: the obvious NULL reads
at 561/606 are guarded, so the fault is a downstream use of a slot whose contents no longer mean what the
consumer assumes.

## Corrected ordering

**The event/barrier paths (561–581, 717–753) must be converted BEFORE or TOGETHER WITH the render path.**
They are the real owners of the slot lifecycle for their stages. **The plan's step 3 becomes step 2**, and
the per-job syncs cannot be dropped type-by-type as originally hoped — **the two mechanisms are coupled
through the same two arrays.**

## Practical shape for the next attempt

Convert **all** writers of `next_job_wait_sync[]` / `last_job_signal_sync[]` in one coherent change (render,
compute/transfer/query, and event/barrier paths), keeping `pvr_update_job_syncs()` only for syncs that are
still per-job; then gate with **`vkrender` first** and the full suite. **That is a larger change than the
four-step plan assumed — which is exactly why the previous attempt failed.**

## Cost of finding this out

One build, one `vkrender` invocation, one `git checkout` revert. **No wedge, no reboot, tree verified clean.**
The gate earned its keep.
