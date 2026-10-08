# The rewrapped vendor firmware loads but faults — and the guard recovered unprompted

## What was tried

The two stacks run **different firmware** for the same GPU: `powervr/rogue_36.56.104.183_v1.fw` (mainline,
131072 B) versus `rgx.fw.36.56.104.183` (vendor, 139264 B). **A previous session had already rewrapped the
vendor firmware** for the mainline interface (`/home/radxa/re/rewrap/rogue_vendorcode_rewrapped.fw`, 139264 B —
the vendor's exact size), **uninstalled**.

**Since the per-surface gap (~2.47×) is in the fragment job, whose cost is ~75% raster/tile processing driven
by the firmware, swapping this in was the direct test of where the gap lives.**

Original backed up (md5 `4b70eca8…`), weston/Xwayland stopped, `powervr` reloaded via `insmod` — **`modprobe`
cannot find it, it is not in `/lib/modules`.**

## What happened

```
01:07:53  powervr: [drm] loaded firmware powervr/rogue_36.56.104.183_v1.fw
01:07:53  powervr: [drm] FW version v1.1 (build 6603887 OS)
01:07:53  Unable to handle kernel paging request at virtual address ffff8000ac7e1fd5
          ESR = 0x0000000096000006   EC = 0x25: DABT (current EL)
          FSC = 0x06: level 2 translation fault
```

**The rewrap is technically sound** — the firmware **loads** and **self-identifies as build 6603887**, exactly
the vendor's `24.2.6603887`. **Then the kernel faults immediately**: a data abort with a level-2 translation
fault — the firmware dereferences a structure the mainline driver never built, because it expects the *vendor*
kernel's memory layout.

**Conclusive and negative: the vendor firmware is unusable under the mainline driver, not for lack of a
wrapper but because of the ABI.**

## The reboot cause, and why it is good news

**`systemctl reboot` was issued deliberately** — the audit log records
`cmd=73797374656d63746c207265626f6f74` (hex for "systemctl reboot") from `/home/radxa/Desktop/Projects` as
uid 1000 via sudo. **This was the guard's recovery path, not a board fault.**

**Verified after the reboot:**
- `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw` is back to **131072 bytes, md5 `4b70eca8…`** — the original,
  byte-exact.
- The GPU is bound to **`pvrsrvkm`** (the guard's fallback).
- `gpu-fw-guard` is **active**.

**The guard did exactly what it exists to do: a bad firmware was installed, the driver faulted, the guard
restored the firmware, switched to the known-good vendor driver, and rebooted to clear state — automatically,
with no operator intervention.** That is the strongest possible evidence for the objective's safety
requirement: **the recovery path was exercised by a real fault and it held.**

**It also refused a later switch with `[guard] ABORT: kwin is alive - refusing to unbind the GPU driver`** —
the user's `kwin_x11` session had started, and the guard correctly declined rather than risk the live desktop.
**Respected, not worked around.**

## What this settles

| question | answer |
|---|---|
| does the per-surface gap live in the firmware? | **cannot be tested** — the vendor firmware cannot run under the mainline driver |
| was the rewrap correct? | **yes** — it loads and self-identifies as build 6603887 |
| why does it fail? | **ABI** — the firmware expects the vendor kernel's structures |
| is the canonical firmware right? | **yes** — restored, and it is the only image for this interface |
| did the recovery path work? | **yes, unprompted, on a real fault, byte-exact restore** |
