# TESTED: offloading GPU work to the CPU loses by 3–26×, measured on lavapipe

## The idea

**If the GPU is slower than the vendor on some work, could the CPU do it instead?** Directly testable — the
board has **8 cores** at load average 1.8 (**~6 idle**), and **lavapipe (llvmpipe) is installed**
(`lvp_icd.json` → `/usr/lib/aarch64-linux-gnu/libvulkan_lvp.so`).

## The measurement

Same probes, same harness, **only the ICD changed**:

| probe | open GPU | vendor GPU | **CPU (lavapipe)** | CPU vs open | CPU vs vendor |
|---|---|---|---|---|---|
| `vkrender` 512 | 1.502 ms | 0.682 ms | **12.657 ms** | **8.4× slower** | **18.6× slower** |
| `vkrender` 2048 | 13.746 ms | 6.488 ms | **40.075 ms** | **2.9× slower** | **6.2× slower** |
| `cstp` | 361.0 M/s | 369.8 | **14.0 M/s** | **25.8× slower** | **26.4× slower** |
| `cstpi` | 70.7 | 144.6 | **8.5** | **8.3× slower** | **17.0× slower** |
| `cstpf` | 88.2 | 145.8 | **11.1** | **7.9× slower** | **13.1× slower** |

**`vkrender` PASSes on the CPU too** — so it's apples to apples.

## The answer

**CPU offload does not help.** The GPU is **3–26× faster** than 8 ARM cores on llvmpipe on every probe. **The
CPU's best case is the 2048 render at 2.9× slower — and even there the vendor GPU is 6.2× faster than the
CPU**, so the CPU path is **never the fastest option available.**

## And the real client makes it worse

**The client frame is already 84% kernel CPU** (client sys 22.7 ms + Xwayland 21.0 ms of a 52 ms frame). **The
CPU is the bottleneck, not spare capacity.** Moving GPU work onto it would compete with the synchronisation
path that is already the dominant cost — **the opposite of what the objective needs.**

> **Confirmed direction: make the GPU path faster, not route around it.** That is what the four shipped PCO
> fixes did, and what the PR-job and sync-timeline levers target.
