# FEX patches for the A733 (local codegen / runtime / compat)

Patches developed against **FEX upstream commit `d848cbb`** (build them from a FEX
checkout at/near that commit; newer FEX may need rebasing). Apply from the FEX
source root:

```sh
git clone https://github.com/FEX-Emu/FEX && cd FEX && git checkout d848cbb
for p in /path/to/fex/patches/*.patch; do git apply "$p" || patch -p1 < "$p"; done
```

> ⚠️ **AI-assisted. Do NOT submit these upstream to FEX** — the FEX project does not
> accept AI-generated contributions. They are shared here as third-party patches you
> apply to your own build. FEX is MIT-licensed, so modified builds are fine.
> **Use at your own risk**; re-run the correctness gate below on your FEX version.

## The patches

### A — Performance (measured wins, A76-pinned, 2026-06-13)
- **`01-unaligned-atomic-backpatch.patch`** (`Arm64.cpp`) — makes FEX's unaligned-atomic
  backpatch actually engage so the site stops SIGBUS-trapping per op. Unaligned `lock`
  atomics **~190× → ~2.5×** (≈1430 → ≈16 ns/op), an **~88×** win. The A733 lacks
  `FEAT_LSE2` (`uscat`), so without this an unaligned atomic faults every time. (See
  fixed register hazards in the patch comments: Xd==Xn, Xn/Xs==x30, Xd==x30.)
- **`02-thread-context-pooling.patch`** (`Core.cpp`, `Context.h`, `LookupCache.h`,
  `CPUBackend.cpp`, `ThreadManager.*`, `SignalDelegator.*`) — per-Context free-lists that
  reuse the per-thread JIT context (LookupCache + CallRetStack + signal alt-stack) instead
  of mmap/munmap per thread; plus `INITIAL_CODE_SIZE` 16MB→1MB (grows on demand). Thread
  create+join **~25× → ~3.7× native** (≈645 → ≈95 µs), a **~6.8×** win. **Concurrency-
  sensitive** — clears only thread-local L1 on release; shared L2 stays coherent.

### B — Bullseye build-compat (needed to compile FEX on Debian 11 / gcc-10)
- **`03-bullseye-build-compat.patch`** — `functional.h` (gcc-10 libstdc++ doesn't mark a
  `std::function` assignment `noexcept`, so FEX's `static_assert(noexcept(...))` fails to
  build — relaxed; still functionally correct), `CodeCache.cpp` (`#define MREMAP_DONTUNMAP`
  absent in glibc-2.31 headers but present in kernel 5.15), `toolchain_x86_64.cmake` (point
  clang at an x86 sysroot when cross-building the thunks). Pure build-enablement.

### C — Chrome-under-FEX + VAAPI + stability (app-specific; read the caveats)
- **`04-chrome-fex-sandbox.patch`** — makes Chrome's **namespace sandbox** work under FEX:
  - `Syscalls.cpp`: `/proc/self/exe` re-exec fix — resolve the app path through the RootFS
    so `execve(/proc/self/exe)` works (was `-ENOEXEC`). **Also fixes plain `fork+execv`** —
    the most generally-useful fix here.
  - `FileManagement.*`: honor a guest `chroot()` by disabling RootFS path redirection.
  - `Passthrough.cpp`: `chroot` hook **+ a `prlimit_64` hack that intentionally *lies*** —
    it ignores Chrome's lowering of `RLIMIT_DATA` (Chrome's sandbox caps would OOM-kill
    renderers under FEX, since host JIT buffers + guest heap share VmData). **This is a
    deliberate correctness violation, explicitly never-upstream.** Remove it if you don't
    run Chrome.
  - Note: `FileManagement` includes a `/tmp/fexchroot.log` debug write — harmless, delete
    if undesired.
- **`05-smc-tracking-nullguard.patch`** — null/`end()` guard in SMC tracking (prevents a
  null-deref crash on untracked/anonymous mappings). General stability; safe.
- **`06-vaapi-thunk-register.patch`** + **`../thunks/libva/`** — a **VAAPI (`libva`) thunk**
  so an x86 process under FEX reaches the *native ARM* `libva` + the `sunxi_ve` VE2 H.264
  encoder (hardware video). Register via the CMakeLists patch; sources in `../thunks/libva/`
  (`Guest.cpp`, `Host.cpp`, `libva_interface.cpp`, `NOTES.md`). Compose with FEX's existing
  libdrm thunk. (The Vulkan GPU thunk is `../vkthunk_render.c`.)

## Correctness gate (run before trusting A/C on your build)
The pooling + backpatch patches must pass a self-modifying-code + thread-churn stress test
(fixed-address SMC across thousands of short-lived threads). A naive pooling impl that
clears the *shared* L2 cache SEGFAULTs under churn — that's the negative control proving
the test has teeth. Build a churn+SMC harness and confirm correct results + no crash.

## Recommended config (no patch needed)
See [`../Config.json.example`](../Config.json.example) — `X87ReducedPrecision` (~18× for
x87-on-64-bit code, trades 80-bit accuracy), `HideHybrid=0` (Chrome pins to A76),
`X87`/TSO settings.
