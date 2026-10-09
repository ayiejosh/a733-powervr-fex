# Full regression: the stack is healthy after the session's work

After ~123 rounds of driver work, experiments and reverts, a full correctness regression to confirm
nothing was left broken.

## End-to-end through the real composited stack

`glmark2-es2 --validate` on weston + Xwayland + zink:

```
[conditionals]  Validation: Success
[function] low  Validation: Success
[function] med  Validation: Success
[loop] false    Validation: Success
[loop] uniform  Validation: Success
```

**Every validation scene succeeds** - the strongest end-to-end check available, exercising the actual
path the objective cares about.

## Compositor-free probes

| probe | result |
|---|---|
| `bda` | PASS (0 failures) |
| `vk13` | VERDICT: PASS |
| `pctest` | PASS (0 failures) |
| `vk16` | VERDICT: PASS |
| `vkrender` | VERDICT: PASS (262144/262144 pixels correct) |
| `inatt` | pass |
| `linfilter` | "no interpolation" - **pre-existing known negative** (no linear filtering of 32-bit float), not a regression |

**The stack is correct and every experiment was reverted cleanly.** Both trees clean: mesa `80788b9`
(36 ahead of main), bench 163 ahead; neither pushed. Driver `powervr`, weston + Xwayland up, kwin absent,
`gpu-fw-guard` active.

## Why a regression instead of another hypothesis

**Four of the previous five hypotheses produced measured negatives** (prologue instructions, DOUTU
sample-rate mode, mtile sample scaling, sample layout, PBE state). Every Mesa-side hypothesis reachable
by configuration or by reading the code has been tested and refuted; both remaining performance terms
bottom out at the closed firmware.

**When probing stops paying, verifying is worth more than another guess.**
