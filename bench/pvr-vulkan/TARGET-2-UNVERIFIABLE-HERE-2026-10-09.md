# Target (2) cannot be verified on this host — 19% is below the 35% measurement floor

## The arithmetic that decides it

**The change is one literal** (`zink_kopper.c:321`, `0` → `2` extra swapchain images), and **the objective
records 43 FPS vs 36 at 0** — **19%**.

**But this session established that wall-clock FPS here cannot resolve below ~35%**: ~65% of a core of
uncontrollable background load (including the agent itself) sits under every measurement. **The one
client-level claim made this session — a 1.27× scene improvement — was withdrawn for exactly this reason.**

**19% < 35%. A correct A/B cannot distinguish the improvement from the noise**, however many runs are
interleaved. **The change cannot be verified here by the only metric that can see it.**

## Why no substitute metric exists

The effect is on the **present path**, where the client **blocks** waiting for a swapchain image. That is
**waiting, not CPU**:

- **sys time** (the harness's proxy, right for the sync lever) **doesn't capture it** — a blocked thread
  accrues no CPU
- **per-job kernel timestamps** measure GPU work; the present wait is not a job
- **FPS** is the only metric that sees it, and it's below the floor

**No available instrument can confirm this change on this board.**

## The honest position

**Do not ship on faith, and do not claim it.** Three options, in order of honesty:

1. **Leave it as the documented one-liner it is** — with the caveat attached, so nobody later mistakes the
   objective's single 43-vs-36 sample for a verified result. **← what is recorded.**
2. **Ship it as a labelled guess** — plausible, upstream-shaped, trivially revertible, **but an unverified
   behavioural change to every zink client**, which this session's discipline has avoided.
3. **Verify on a host with a lower FPS floor** (no agent, no sync daemon) — **not available here.**

**Option 1 is what this entry does.** The change is one `sed` away, its rationale is recorded, and **the reason
it isn't landed is a measurement limit, not a doubt about the code.**
