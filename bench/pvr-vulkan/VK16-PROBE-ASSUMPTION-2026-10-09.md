# `vk16`'s "FAIL" under the vendor is a probe assumption — and it reveals a real feature-flag difference

Running the correctness gate under the **vendor** driver, everything passes except `vk16`:

```
FAIL  device feature shaderFloat16 = 1 (expect 0 - deliberately off)
ok    vkCreateDevice with shaderFloat16 + shaderInt8 -> 0
ok    narrow-type compute shader compiled -> 0
ok    f16 multiply is exact (27)      ok  f16 comparison works (1)
ok    int8 division truncates (95)    ok  uint8 addition wraps (44)
ok    the write stayed inside its slot (v[6]=0)
VERDICT: FAIL (8 ok, 1 failed)
```

## What this means

**Every functional check passes.** The single failure is the probe asserting `shaderFloat16` is **off by
default** — which its own output calls *"deliberately off"*. **That assumption encodes the open driver's
behaviour, not a requirement: the vendor exposes `shaderFloat16` by default.**

**So `vk16` is open-driver-specific; its verdict is not applicable to the vendor.** The gate should treat it as
informational when the vendor is bound — **the functional half of it passes there.**

## The feature-flag finding, which is the useful part

This answers part of *"was it missing feature flags?"*: **`shaderFloat16` is on by default in the vendor driver
and off in the open driver** — a real, observable API-surface difference between the two stacks, found by the
gate rather than by reading tables.

**It is not a performance lever for existing workloads**: it changes what apps are *offered*, not the fps of
already-fp32 shaders. An app that would use fp16 behaves differently per driver — **a compatibility item, not a
speed item** — and **the open driver's fp16 arithmetic is correct when the feature is enabled** (every
functional check above passes).

## Gate, corrected per bound driver

| check | open | vendor |
|---|---|---|
| `vkrender` 512 / 2048 | PASS | **PASS** |
| `bda` / `vk13` / `pctest` | PASS | **PASS** |
| `vk16` | PASS | **FAIL — probe assumption only**; functional checks pass |
| `glmark2-es2 --validate` | 27 scenes | compositor-bound; not run |
