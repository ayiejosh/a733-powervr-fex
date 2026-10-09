# The CPU/GPU question, both directions — measured

## Direction 1: GPU work onto the CPU — loses 3–26×

Same probes, only the ICD changed (lavapipe on 8 cores):

| probe | open GPU | vendor GPU | CPU | CPU vs open |
|---|---|---|---|---|
| `vkrender` 512 | 1.502 ms | 0.682 | 12.657 | **8.4× slower** |
| `vkrender` 2048 | 13.746 | 6.488 | 40.075 | **2.9× slower** |
| `cstp` | 361.0 M/s | 369.8 | 14.0 | **25.8× slower** |
| `cstpi` | 70.7 | 144.6 | 8.5 | **8.3× slower** |
| `cstpf` | 88.2 | 145.8 | 11.1 | **7.9× slower** |

**The CPU is never the fastest option available** — its best case (2048 render) is 2.9× slower than the open
GPU, and the vendor GPU is 6.2× faster than the CPU there.

## Direction 2: CPU work that shouldn't exist — **84% of the frame**

**The client frame is 84% kernel CPU** (client sys 22.7 ms + Xwayland 21.0 ms of 52 ms), from **~190 syncobj
ioctls per frame**. **Per-job host bookkeeping — not compute.**

**The vendor's target: `pvr_srv_sync` at 0 ioctls/op where the open path costs 1.**

## Direction 3: CPU work onto the GPU — not possible

**Synchronisation must stay host-visible** (client and compositor both wait on it), so it can't be offloaded
into GPU work. **What can change is its granularity**: one timeline point per job type instead of a
create/destroy pair per job.

## The complete answer

| direction | answer |
|---|---|
| GPU → CPU | **loses 3–26×**; the CPU is already the bottleneck |
| **CPU work that shouldn't exist** | **84% of the frame**; **fix = granularity, not offload** |
| CPU → GPU | **not possible**; sync must stay host-visible |

> **The principle is right; the surface is different. The win is not moving work between the units — it is
> removing per-job host work entirely.** That is exactly what the sync-timeline lever does, and why it is the
> largest remaining item.
