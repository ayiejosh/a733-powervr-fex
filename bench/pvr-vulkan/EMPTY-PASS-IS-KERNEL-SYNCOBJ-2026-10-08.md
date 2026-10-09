# The empty-pass overhead is KERNEL syncobj time - target (3) is the cause

## Verification first

An earlier sample read 14.4 ms/frame for the empty pass and looked wrong. It was: repeated runs at
20/100/500/2000 iterations give the open driver a **consistent 0.73-0.97 ms/frame** (wall time agrees),
and the vendor **0.002-0.006 ms** (wall 0.055 s for 2000 frames). The 14.4 ms sample was contaminated.

## The split

```
MODE=empty 2048 x 2000:  wall 2.086 s   user 0.176 s   sys 0.912 s   (0.968 ms/frame)
wchan: drm_syncobj_array_wait_timeout.constprop.0
```

| | open | vendor |
|---|---|---|
| empty pass | 0.968 ms/frame | **0.003 ms/frame** |
| of which kernel (`sys`) | **0.456 ms/frame** | ~0 |

**44% of the time is in the kernel, blocking on DRM syncobj.**

## Why this is the strongest link so far

* The per-pass overhead - **74x the vendor's** - is dominated by **DRM syncobj kernel time**. That is
  objective target (3): "vk_sync as DRM syncobj operations (one ioctl each) where the vendor uses a
  driver-native sync type". **The architectural difference is now tied to a concrete per-pass cost.**
* It **unifies three earlier findings**: the fixed per-frame cost (0.85 vs 0.52 ms), the 15 syncobj
  ioctls/frame, and this empty-pass figure are the same thing seen three ways.
* The vendor's 0.003 ms means its native sync path is essentially free. **The gap is not extra GPU work
  for an empty pass - it is kernel work the vendor does not do.**

## What this does NOT mean

**Timeline-backing is still not the fix** - measured directly last round, it made the ioctl count
worse (create/destroy traded for resets, waits +50%). The cost is the *number* of DRM syncobj
operations and their per-ioctl overhead, not the binary-vs-timeline representation. Options:
(a) fewer syncobj operations per submit, or (b) a driver-native sync type in the mainline module,
which does not exist. Only (a) is in Mesa's hands.

## Next

An empty pass with no work to order should not need 3 creates, 3 transfers, 3 destroys and 2 waits.
Count what they are for and whether any are avoidable - now the highest-value question, because it is
per pass and every frame makes several passes.
