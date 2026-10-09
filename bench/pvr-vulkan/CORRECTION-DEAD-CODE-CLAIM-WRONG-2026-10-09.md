# CORRECTION: my "dead code" root cause was WRONG — the mainline name is `powervr`, not `pvr`

## What I asserted without checking

I recorded that `PVR_DRM_DRIVER_NAME` is probably `"pvr"`, so the first branch would match the vendor's
`name=pvr` and make the srv branch unreachable. **I wrote it as "if", then recorded the whole chain as though it
were established. It's wrong:**

```
pvr_winsys.h:46:  #define PVR_DRM_DRIVER_NAME "powervr"
```

**The mainline name is `"powervr"`. The vendor reports `name=pvr`. They differ** — so the first branch does
**not** match, and **the `else if (PVR_SRV_DRIVER_NAME)` branch is the one taken.**

## What that means

**The srv winsys IS selected.** The failure is **inside `pvr_srv_winsys_create()` or later** — the srv
device-info / bridge path — **not branch ordering, and not the build option.**

**Fourth time this session a mechanism I reasoned from reading code was wrong**, with the identical pattern: **a
plausible-looking source, asserted without testing it against the running system.**

## Still true from the previous entry

- vendor kernel presents `name=pvr  version=24.2.6603887`
- mainline winsys calls `DRM_IOCTL_PVR_DEV_QUERY`, which the vendor kernel doesn't implement
- open ICD reports **0 physical devices** against the vendor kernel
- `imagination-srv=true` compiles the srv winsys in (69 define occurrences)

**Now known false:** that the mainline branch matches first, and that the srv path is dead code.

## Correct next step

**Trace `pvr_srv_winsys_create()` and `pvr_srv_winsys_device_info_init()`** — the srv path *is* taken, so one of
them fails. **Instrument with a log at each early-return; `MESA_DEBUG=1` produced nothing.**
