# components.sh — probe every small component, see what works

One command, one table, one verdict per component. Answers "what's working / what isn't" at a glance instead
of inferring it from a performance number.

## What it checks

| group | checks |
|---|---|
| **kernel / modules** | `pvrsrvkm`, `powervr`, `drm_shmem_helper`, `drm_exec`, `gpu_sched` loaded (size + refs); running kernel; **`powervr.ko` vermagic vs the running kernel** |
| **device / binding** | which driver owns `1800000.gpu`; `/dev/dri/card0`, `/dev/dri/renderD128`; the apphint debugfs knobs |
| **firmware** | both images with **size + md5** (mainline `rogue_*` and vendor `rgx.fw`) |
| **userspace** | both Vulkan ICDs and **whether the library each one names actually resolves**; both driver libraries with sizes |
| **mesa source** | the four landed fixes present (`max_unroll_iterations`, immediate hoisting); git state (commit, dirty files, commits ahead) |
| **instruments** | tracepoints the *bound* driver should expose; trace writability; whether a compositor is blocking a switch; SDDM; `gpu-fw-guard` |
| **harness** | `harness.py`, `sweep.sh`, `ab.sh` present and executable; record count in `harness-log.jsonl` |

## Current output

**24 OK, 6 needs attention** — every one of the six a true statement, not a defect:

| item | verdict | why that is correct |
|---|---|---|
| `module powervr` | `--` | the vendor driver is bound, so the open one should not be loaded |
| `pvr apphint debugfs` | `--` | that tree belongs to the open driver |
| `tracepoints gpu_scheduler` / `pvr_fence` | `--` | each driver exposes its own; only one can be present |
| `trace control writable` | `WARN` | needs root; the harness uses sudo |
| `compositor` | `WARN` | `kwin_x11` alive, so switching is blocked |

## Three false positives this found in ITSELF, all fixed

A checker that cries wolf is worse than none, so each of these was chased down:

1. **`libVK_IMG.so` reported unresolvable** — it exists as `/usr/lib/libVK_IMG.so` → symlink. **The check tested
   the bare soname as a literal path.** Fixed by searching the standard library directories and the loader
   cache.
2. **`powervr.ko` vermagic reported empty** — `modinfo` prints **nothing** for this `.ko`, though
   `strings` contains `vermagic=6.6.98-5-aw2511 SMP mod_unload aarch64`. Fixed by falling back to the string
   table. **This matters: an unreadable vermagic is exactly how an out-of-kernel module goes unnoticed.**
3. **Driver-dependent checks reported `FAIL`** — a missing `gpu_scheduler` tracepoint under the vendor driver is
   correct, not a failure. They now report `--` and say why.

## Why this is the companion to the harness

The harness measures **how fast**; this measures **whether the thing being measured is the thing you think it
is**. Every silent-wrong-answer bug found this session — the mislabelled driver, the `insmod` dependency
failure, the out-of-kernel `.ko` — would have been visible here in one line.
