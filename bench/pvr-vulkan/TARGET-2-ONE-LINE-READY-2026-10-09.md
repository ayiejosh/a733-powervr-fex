# Target (2) is a ONE-LINE change, measured at 19% by the objective, left ready to test

## The finding

`src/gallium/drivers/zink/zink_kopper.c:320`:

```c
   cswap->scci.minImageCount = cdt->caps.minImageCount +
      (getenv("ZINK_EXTRA_IMAGES") ? atoi(getenv("ZINK_EXTRA_IMAGES")) : 0);
```

**The default is 0 extra swapchain images**, and the objective's own target (2) records:

> *"The client blocks 23 ms per frame acquiring a swapchain image (weston's cycle is 53 ms for 3 images).
> Testing whether more swapchain images amortise the release latency; **ZINK_EXTRA_IMAGES=2 gave 43 FPS vs 36
> at 0 extra**."*

**A 19% client-level improvement, in scope (zink), taken by a single literal: `0` → `2`.**

## What was done, and why it was reverted

**Changed the default to 2, built it (full build 17/17, no errors). Then reverted.**

**Reason: verifying it needs the client running — weston + Xwayland + glmark2 through zink — and my context
budget is spent.** A client-facing default change that *compiles* is **not** verified: raising `minImageCount`
changes swapchain depth, memory use and present latency, and **43-vs-36 is a single earlier measurement that
has not been reproduced.** Committing it unverified would repeat the timeline mistake — shipping on green
probes.

## Ready to test, exactly

```sh
# 1. the literal
sed -i 's/getenv("ZINK_EXTRA_IMAGES")) : 0)/getenv("ZINK_EXTRA_IMAGES")) : 2)/' \
    src/gallium/drivers/zink/zink_kopper.c
ninja -C build

# 2. the client test - the only one that can see it
#    close desktop, switch to open, start weston + Xwayland, then interleave:
#      ZINK_EXTRA_IMAGES=0 glmark2-es2 ...   vs   ZINK_EXTRA_IMAGES=2 glmark2-es2 ...
#    repeat with the default changed to confirm it behaves as the env var
```

**Gate: probes → weston must come up → the client A/B above.** And since client FPS here **cannot resolve
below ~35%**, the 19% needs **interleaved runs and more than 3 samples** — or a non-wall-clock metric for the
present path, if one exists.
