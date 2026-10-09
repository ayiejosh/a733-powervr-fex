# The host-side PR TODO is a refactor, not a skip — the driver says `/* Massive copy :( */`

## The check

Before treating the host-side PR TODO as a quick win, verified whether the PR state depends on the fragment
state:

```c
   /* Massive copy :( */
   *state = *frag;

   assert(state->fw_stream_len >= pbe_reg_byte_offset + sizeof(job->pr_pbe_reg_words));
   memcpy(&state->fw_stream[pbe_reg_byte_offset], job->pr_pbe_reg_words,
          sizeof(job->pr_pbe_reg_words));
   pvr_csb_pack((uint32_t *)&state->fw_stream[eot_data_addr_byte_offset], ...);
```

**The PR state is a wholesale struct copy of the fragment state, then two patches.** The fragment's command
stream is **already built** by `pvr_render_job_ws_fragment_state_init()` before this runs — and when
`!run_frag` that stream is never submitted (`[2]` is gated on `has_fragment_job`).

## The host-side saving is real, but it is NOT a one-line skip

**You cannot simply skip the fragment init**, because the PR init reads its output. **The PR path must build
its own stream directly** — exactly what the TODO says ("setup the pr state directly if `!job->run_frag`").
**The driver's `/* Massive copy :( */` comment shows the authors know the shape of the problem.**

**The prize: a whole fragment command stream built and then copied, for a job that produces nothing when PRs
aren't needed.** Host CPU — and host CPU is one of the two levers.

## For the handoff

**Do not start from "skip the fragment setup"** — it will not compile, and the dependency will look like the
obstacle. **Correct starting point: give the PR path a direct builder for its own stream, then drop the
fragment init when `!run_frag`.** Both TODOs in `pvr_arch_job_render.c` are the same job seen from two ends.

## Reminder of the gate

**Probes → weston must come up → glmark2 via zink validates.** The probes alone are blind to driver-level
breakage — demonstrated twice.
