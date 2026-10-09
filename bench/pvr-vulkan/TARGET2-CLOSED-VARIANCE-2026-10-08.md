# Target (2) closed by interleaved A/B - and measurement variance is ~25%

The objective's target (2) claims `ZINK_EXTRA_IMAGES=2` gave "43 FPS vs 36 at 0 extra". My later
measurement showed no effect, so re-tested properly: **interleaved** A/B, three rounds, same command and
conditions, background load checked first (`syncthing` 33%, `MainThread` 27.5%).

| round | `ZINK_EXTRA_IMAGES=0` | `ZINK_EXTRA_IMAGES=2` |
|---|---|---|
| 1 | 60 FPS | 47 FPS |
| 2 | 48 FPS | 55 FPS |
| 3 | 51 FPS | 60 FPS |

**The ordering flips every round — the difference is noise.** Target (2) is closed: adding swapchain
images does not improve frame rate on this stack.

## More important: measurement variance is ~25%

Under **identical** conditions the same configuration measured **47–60 FPS** — a ±13 FPS spread, ~25%.
Consequences for this session's record:

* **Any single-sample comparison in this session's history is unreliable at the ±25% level**, including
  every "X vs Y FPS" number taken from one run of each. Differences under ~25% should be treated as
  unproven.
* **The interleaved method is mandatory, not merely good practice.** Three interleaved rounds were enough
  to see this one is noise; a sequential A-then-B would have "found" a 13 FPS regression or improvement
  depending only on which ran first.
* **Measurements that survive** are those with effects far above 25%, and those taken from **syscall
  counts or driver-internal traces** rather than wall-clock FPS.

## Revised confidence in the session's numbers

| finding | effect size | confidence |
|---|---|---|
| render is 100% per-surface, 3.4x | 4.4x | high - far above noise |
| empty pass 74x (syncobj) | 300x | high |
| present = WSI waits, vendor has none | 20x | high |
| per-pass kernel time, 15 ioctls/frame | counts, not FPS | high |
| **`ZINK_EXTRA_IMAGES` helps** | claimed 1.2x | **refuted - noise** |
| MSAA growth 8.16x vs 3.06x | 2.7x | medium - above noise but single-sample |

**Target (2) is now closed**, and it was the last objective target that could plausibly have been real
without a large effect size.
