# The linear-layout penalty is inherent to both drivers - not the deficit

The driver is linear-only for images, so the WSI hands out LINEAR render targets. Hypothesis: a linear
target makes the PBE's writes less efficient, explaining the 3.2x PBE-write deficit. Split by tiling:

| 2048, s1, open | optimal | linear | delta |
|---|---|---|---|
| Tiler (DISCARD) | 7.358 ms | 8.644 ms | +17% |
| Shader | 0.523 ms | 0.365 ms | ~0 |
| **PBE write** | **6.250 ms** | **6.304 ms** | **+1%** |
| total | 14.131 ms | 15.313 ms | +8.4% |

**And the vendor pays the same penalty - more, proportionally:**

| 2048, s1 | optimal | linear | penalty |
|---|---|---|---|
| open | 14.131 ms | 15.313 ms | **+8.4%** |
| vendor | 5.662 ms | 6.590 ms | **+16.4%** |

## Established

1. **The PBE write is layout-independent** (6.250 vs 6.304 ms). The linear-only constraint does not
   explain the 3.2x PBE deficit. Hypothesis refuted.
2. **The linear penalty is inherent hardware behaviour** - the vendor pays it too, and pays *more*
   proportionally. Not a driver defect; the WSI's linear-only limitation is a cost of the platform, not
   a Mesa performance bug.

Practical note: **a real client's render target is linear and costs ~8-16% on the render, on both
drivers.** Worth knowing, not something Mesa can fix.

## Tally

**Sixteen hypotheses tested and refuted.** The PBE write (3.2x) and the Tiler MSAA excess remain, both
at the closed-firmware boundary; the layout was the last Mesa-side variable not yet measured.
