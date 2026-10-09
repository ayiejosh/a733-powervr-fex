# The PR job is a full fragment-shaped submit that does nothing when PRs aren't needed

## The finding

`pvr_render_job_ws_fragment_pr_init_based_on_fragment_state()` in `pvr_arch_job_render.c` builds the PR job's
state **from the fragment job's state**:

```c
static void pvr_render_job_ws_fragment_pr_init_based_on_fragment_state(
   const struct pvr_render_ctx *ctx, struct pvr_render_job *job,
   struct vk_sync_wait wait, struct pvr_winsys_fragment_state *frag,
   struct pvr_winsys_fragment_state *state)
{
   const uint32_t pbe_reg_byte_offset = pvr_frag_km_stream_pbe_reg_words_offset(dev_info);
   const uint32_t eot_data_addr_byte_offset = pvr_frag_km_stream_pds_eot_data_addr_offset(dev_info);
   ...
   memcpy(&state->fw_stream[pbe_reg_byte_offset], ...);   /* patch PBE registers */
   pvr_csb_pack((uint32_t *)&state->fw_stream[eot_data_addr_byte_offset], ...);  /* patch EOT addr */
}
```

**A full fragment-shaped command stream with two offsets patched.** And `pvr_drm_job_render.c:587` says the
job is *"scheduled after the geometry job, but no PRs will be performed, as they aren't needed"* — **so in the
common case this full fragment-shaped pass produces nothing.**

## It is the worst stage, and it is Mesa-side

| job | open | vendor | ratio |
|---|---|---|---|
| geometry / TA | 0.35 ms | 0.82 ms | **0.43× — open WINS** |
| **PR (partial render)** | **9.31 ms** | **2.31 ms** | **4.03×** |
| fragment | 13.01 ms | 5.99 ms | 2.17× |

**The worst ratio in the decomposition — worse than the fragment job whose shader cost this session already
halved.** And unlike the fragment job, **its cost is not shader-bound**: the PR stream *is* the fragment
stream, so the difference is per-tile/per-job fixed work.

## The driver's own TODOs already predict this

> *"See if it's worth avoiding setting up the fragment state and setup the pr state directly if
> `!job->run_frag`. For now we'll always set it up."*
>
> *"In some cases we could eliminate the pr and use the frag directly in case we enter SPM. There's likely
> some performance improvement to be had there. For now we'll always setup the pr."*

**The authors expect an improvement here and have not taken it. The measured 4.03× is the size of the prize.**

## Directions, in order of safety

**1. When `!job->run_frag`, set up the PR state directly** instead of building the fragment state first (the
first TODO). **Host-side only, changes no GPU behaviour.** — *the safe first step*
**2. Eliminate the PR job when the geometry cannot have overflowed** so no PR can be needed. **Riskier**: the
driver cannot know in advance what the firmware decides, so this needs a defensible conservative test.

**Gate for either: the probe suite via `harness.py`, then weston must come up, then glmark2 via zink
renders and validates** — the same three steps, because the probes alone are blind to driver-level breakage.
