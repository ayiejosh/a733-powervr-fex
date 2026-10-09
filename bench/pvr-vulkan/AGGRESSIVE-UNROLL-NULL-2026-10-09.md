# The aggressive unroll limit is a null below 1024 iterations — and setting it high is unsafe

## What I found and tested

NIR offers **three** unroll limits, not one:

```c
unsigned max_iter = shader->options->max_unroll_iterations;
if (shader->options->max_unroll_iterations_aggressive && can_pipeline_loads(loop))
   max_iter = shader->options->max_unroll_iterations_aggressive;
if (shader->options->max_unroll_iterations_aggressive && li->flattens_all_control_flow)
   max_iter = shader->options->max_unroll_iterations_aggressive;
```

**The aggressive limit applies only when the loop flattens all control flow** — i.e. when full unrolling
eliminates every branch. **NIR applies it only when the whole body flattens, so a large body that would bloat
is not affected.** That looked like the size-aware policy the cliffs have been asking for, and **PCO didn't
set it** — so I added it at 32768 and measured.

## The measurement

| probe | limit 1024, no aggressive | **+ aggressive 32768** |
|---|---|---|
| `cstpi512` (512 iters) | 5.1 M/s | **5.1** |
| `cstpi128` (128 iters) | 19.6 | **19.6** |
| `cstpi` (32 iters) | 71.6 | **70.6** |
| `cstpf` (32 iters) | 87.7 | **85.1** |
| `vkheavy` (real 32-iter shader) | 255.5 ms | **255.5 ms** |

**Nothing changed.** The reason is straightforward: **512 < 1024, so the normal limit already unrolls every
loop any probe contains.** The aggressive limit only takes effect **above 1024 iterations**, which nothing
here reaches.

## Why I reverted rather than kept it

**Setting it to 32768 is not free.** It would fully unroll a *long* loop that flattens — producing an
enormous shader and the instruction-cache pressure that comes with it. **That cost is invisible to these
probes** — which is precisely why a change with **no measured benefit** and a **real unmeasured risk** should
not ship.

**Reverted; default restored and verified** (`cstpi512` 5.1, `vkrender` 2048 PASS).

## What this leaves

**The cliffs are a property of a fixed threshold.** The machinery to do better exists — `aggressive`, plus
`force_unroll` and per-loop `unroll`/`dont_unroll` control — **but using it well needs a policy based on trip
count and body size: a pass, not a constant.**

Until that exists, **the limit is set by the longest loop actually measured (1024)**, and **each raise should
come with a probe showing the cliff it removes** — which is how the last three were justified.
