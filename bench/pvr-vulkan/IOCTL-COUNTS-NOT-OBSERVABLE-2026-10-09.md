# ioctl counts are NOT observable on this kernel — the proxy is CPU sys time

## Every mechanism, tried in order

The goal asks the harness to observe **ioctl counts** — the one observable it still lacks:

| mechanism | result |
|---|---|
| syncobj / drm ioctl **tracepoints** | **none exist** |
| **`kprobe_events`** | **not available** — file does not exist |
| **`function_profile_enabled`** | **Permission denied**; `trace_stat/` missing |
| **`available_tracers`** | **empty** — ftrace tracers not compiled in |
| `strace` | already blocked (`ptrace: Operation not permitted`) |

**The kernel symbols exist** — `drm_ioctl`, `drm_syncobj_create_ioctl`, `drm_syncobj_destroy_ioctl`,
`drm_syncobj_transfer_ioctl` are all in `/proc/kallsyms` — **so this is an instrumentation limit of the kernel
build, not a driver limitation.**

**Event tracepoints DO work** (`tracing_on`, `trace`, the per-driver `gpu_scheduler`/`pvr_fence` groups) —
that's why `harness.py` reports per-job durations. **It is specifically the tracer/kprobe machinery that is
absent**, so counting is impossible while timing is not.

## Consequence for the harness and the lever

**CPU sys time is the proxy, and the harness already reports it** (`getrusage(RUSAGE_CHILDREN)`, as user/sys
with the kernel share). **That is the right proxy here** — the cost being attacked is host-side kernel time,
and sys time measures exactly that without per-ioctl attribution.

**Not available: attributing kernel time to specific ioctl types.** The 42 CREATE + 42 DESTROY + 56 TRANSFER
breakdown came from an earlier measurement and **cannot be re-derived on this kernel** — so treat those as
historical and use **sys time for the delta**.

**This closes the goal's "ioctl counts" observable with a reason rather than leaving it an omission.**
