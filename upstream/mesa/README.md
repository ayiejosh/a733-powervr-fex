# Mesa: the pco optimisation series, in submittable form (NOT sent)

The PowerVR Compiler Optimiser work that `mesa/open-driver-series/` carries as 41 separate patches,
squashed into the two-commit form a Mesa merge request wants.

| patch | goes to | state |
|---|---|---|
| [`0001-nir-add-a-load_desc_checks_pco-intrinsic.patch`](0001-nir-add-a-load_desc_checks_pco-intrinsic.patch) | `mesa/mesa` (MR) | ready, blocked on account verification |
| [`0002-pco-instruction-selection-scheduling-control-flow-an.patch`](0002-pco-instruction-selection-scheduling-control-flow-an.patch) | `mesa/mesa` (MR) | ready, blocked on account verification |

Base commit: upstream `9a2b374`. 23 files, +3333/-231.

## Why it exists separately from the series

`open-driver-series/` is the **record** - the actual commits in order, as `format-patch` output. This is
the **submission** - same code, changed in three ways to survive review:

1. **clang-format clean.** Mesa's CI format-checks `src/imagination/**/*` and runs `git-clang-format`
   diff-scoped, pinned to `clang-format-19` (`.gitlab-ci/container/debian/gitlab-ci.yml` sets
   `LLVM_VERSION: 19`). The series' patches predate this: **248 of our changed lines needed
   reformatting**, across 11 files. Applied here; now **0 violations**.
2. **`Generated-by: LLM` trailers.** Required by Mesa's `AGENTS.md` for AI-written code. The series'
   patches do not carry them.
3. **Squashed into two commits**, one per subsystem - a prerequisite NIR intrinsic, then the compiler
   change - so each is independently buildable and the sequence bisects.

## Verified

| check | result |
|---|---|
| applies to pristine `9a2b374` | clean (both) |
| each commit builds, `-Werror=true` | pass (verified separately, so it bisects) |
| clang-format (v19, diff-scoped) | **0 violations** |
| `Generated-by: LLM` present | both |
| local-only instrumentation leaked | none |

Measurements are carried in `0002`'s commit message: `vkheavy` 858.5->255.8 ms (3.36x), `cstpi`
26.6->72.8 M/s, `cstpf` 29.6->87.9 M/s, against the `cstpin` control that does not move.

## Why it is not sent

Not a Mesa problem. freedesktop's GitLab refuses to let the account create **any** project:

```
Limit reached You cannot create projects in your personal namespace. Contact your GitLab administrator.
```

The instance's anti-spam restriction on new accounts (the banner says so). Username and token are both
valid - only project creation is refused, so no fork exists and no MR can be opened. Known state: a QNX
contributor hit the identical error in March 2026 and was pointed at the "Account verification request"
process (`freedesktop/freedesktop` issue #2486).

**To unblock:** file an Account verification request, then fork and `git am` these two patches.
