# Timeline step 2 (GEOM) executed — and caught by the gate

## What was done

Executed the mechanical spec: 6 edits to `pvr_arch_queue.c`

1. GEOM create → `queue->job_value[PVR_JOB_TYPE_GEOM]++; geom_signal_sync = queue->job_sync[PVR_JOB_TYPE_GEOM];`
2. Both GEOM waits → the persistent sync with `.wait_value = queue->job_value[PVR_JOB_TYPE_GEOM] - 1`
3. The GEOM signal → `.signal_value = queue->job_value[PVR_JOB_TYPE_GEOM]`
4. The `pvr_update_job_syncs(..., PVR_JOB_TYPE_GEOM)` call removed
5. The `err_destroy_geom_sync` label's `vk_sync_destroy(geom_signal_sync)` **removed** — it would have freed a
   persistent sync, the exact use-after-free the plan warned about

**It built cleanly, and `geom_signal_sync` was verifiably never destroyed afterwards.**

## The gate caught it

| probe | result |
|---|---|
| `bda` | PASS (0 failures) |
| `vk13` | PASS |
| `pctest` | PASS (0 failures) |
| `vk16` | PASS |
| **`vkrender` 512 and 2048** | **no output — SIGSEGV (sig=11) before rendering** |
| `vktex`, `mrt` | no output |

`dmesg` showed **no GPU error and the driver stayed bound** — a **userspace crash, not a wedge**.

## Reverted and verified

`vkrender` 512 = 262144/262144 correct, 2048 = 4194304/4194304 correct, `bda` PASS(0), tree clean.

## What the next attempt must do first

**The spec's assumption — that the render path can be converted in isolation — is wrong.** The crash happens
*before* rendering, and `job_sync[]` **is** populated (`pvr_queue_init` at `pvr_arch_queue.c:81` creates
them), so the fault is a **hidden dependency on the slots the conversion stops filling**:

- `queue->next_job_wait_sync[PVR_JOB_TYPE_GEOM]`
- `queue->last_job_signal_sync[PVR_JOB_TYPE_GEOM]`

Removing the render path's `pvr_update_job_syncs()` call means **nothing sets them any more**, while other
code may still read them — the event/barrier paths, and `pvr_drm_job_render.c`, which my earlier notes record
as having **asserted against timeline syncs at 8 sites** (asserts are compiled out in this release build, so
a violation crashes instead of reporting).

**So the next attempt should begin by enumerating every reader of `next_job_wait_sync[]` /
`last_job_signal_sync[]` for that type** — not by assuming the render path is self-contained.

**The `vkrender` gate is what caught this in one command, and it should stay the first check after any edit
to the sync path.**
