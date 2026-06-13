---
name: Benchmark result
about: Share numbers from bench/ so we build a cross-board baseline
title: "[bench] <board> — <what you ran>"
labels: benchmark
---

**Board / SoC:** (e.g. Radxa Cubie A7A, A733 sun60iw2)
**Kernel:** `uname -r`
**DDK:** `strings /usr/lib/libVK_IMG.so* | grep -m1 24.`
**Cooling / ambient:** (fan? passive? room temp)

**Harness + command:** (which file in `bench/`, exact args)

**Numbers:**
```
paste output here
```

**Anything notable** (governor, throttling, different DDK/BSP, etc.):
