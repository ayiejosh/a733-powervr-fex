# The PR job is 72% of the open fragment pass but only 39% of the vendor's

## The proportion

| driver | PR job | fragment job | **PR as % of its own fragment** |
|---|---|---|---|
| **open** | 9.31 ms | 13.01 ms | **72%** |
| **vendor** | 2.31 ms | 5.99 ms | **39%** |

**Absolute ratio 4.03×; the fragment job's ratio is 2.17×.** The PR job has the worse ratio **because the open
driver's PR pass is proportionally far more expensive relative to its own fragment work than the vendor's is.**

## What that implies

**Both drivers submit a PR job that "performs no PRs when they are not needed."** If the vendor's costs **39%**
of its fragment pass while the open's costs **72%**, **the vendor is doing less work in that job.**

**Most likely reason is structural:** the vendor's kernel and firmware are co-designed, so it can know no PR
is needed and **shorten the pass or early-out** — whereas the open driver **always runs the full
fragment-shaped stream it copied.**

## Why this changes the fix direction

**The host-side TODO ("setup the pr state directly if `!job->run_frag`") only stops *building* the stream.** It
saves host CPU and **cannot touch the 72%-vs-39% GPU difference.**

**The direction that addresses the measured 4.03× is: make the PR job cheap when no PR is needed** — a smaller
tile range, an early-out, or skipping the submission where no PR can be shown possible. **That is the GPU-side
TODO, and this proportion is the evidence it is the right one:** the vendor demonstrates a
**39%-of-fragment PR pass is achievable on the same hardware.**

**So the PR job now has a target number, not just a ratio: close 72% → 39%, worth roughly half the PR job's
cost.**
