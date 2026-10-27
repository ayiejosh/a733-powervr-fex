# pco optimisation series (prepared for Mesa, not submitted)

Two commits improving the PowerVR Compiler Optimiser. Verified, portable, **not submitted**.

| file | scope |
|---|---|
| `0001-nir-add-a-load_desc_checks_pco-intrinsic.patch` | `src/compiler/nir/` - prerequisite |
| `0002-pco-instruction-selection-scheduling-control-flow-an.patch` | 22 files in `src/imagination/pco/` |

Base commit for both: upstream `9a2b374`.

```sh
git am /path/to/open-driver-perf/upstream-patches/0*.patch    # in a Mesa checkout at that base
```

## Verified state

* applies clean to the base; **23 files, +3333/-231**
* each commit builds independently with `-Werror=true` (bisectable)
* **0 clang-format violations** (Mesa's pinned v19, checked diff-scoped with `git-clang-format`)
* `Generated-by: LLM` trailer present on both (Mesa's `AGENTS.md` requires it)
* no local instrumentation leaked (`PVR_CSB_*` absent)

## Measurements carried in the commit message

```
probe      unit     before      after      gain     vs vendor
vkheavy    ms       858.5       255.8      3.36x    4.77x -> 1.42x
cstpi      M/s      26.6        72.8       2.74x    5.53x -> 2.02x
cstpf      M/s      29.6        87.9       2.97x    4.93x -> 1.66x
```
Control probe `cstpin` (register-operand form) does not move, which is what makes the gain attributable.
`glmark2-es2 --validate`: 27 scenes OK.

## Why it is not submitted

Mesa is not the blocker. **freedesktop's GitLab refuses to let the account create any project**:

```
Limit reached You cannot create projects in your personal namespace.
Contact your GitLab administrator.
```

This is the instance's anti-spam restriction on new accounts, not a token or username problem
(username `ayiejosh` is valid; the API returns 200 for it). It is a known, documented state - a QNX
contributor hit the identical error in March 2026 and was told the same thing.

**To unblock:** file an "Account verification request" issue at
<https://gitlab.freedesktop.org/freedesktop/freedesktop/-/issues/new>, following the shape of
[issue #2486](https://gitlab.freedesktop.org/freedesktop/freedesktop/-/issues/2486) - state that you
cannot fork, what you want to contribute, and tick the code-of-conduct and no-spam declarations.
Once an admin grants internal-user status, the fork works and the series can be sent.

Local branch holding these commits: `upstream-pco` in `/home/radxa/mesa/mesa-main`.
