# box64 on A733 (trixie / 6.6)

[box64](https://github.com/ptitSeb/box64) runs x86-64 Linux userspace on ARM64.
Nothing here is forked. On the trixie stack box64 is **built from source** and used
**explicitly** (`box64 ./app`) — bare `./prog.x86_64` execs go to **FEX** instead (see
`../fex/` and the binfmt note below).

## Build box64 0.4.3 from source (recommended)

The Debian-packaged box64 on this board is **0.3.4**, which hard-crashes on `clone3`
(syscall 435) — `Unsupported Syscall 0x1B3h`, so threaded x86-64 programs do not even
spawn. Building **0.4.3** fixes clone3 and is ~9% faster on the CPU bench.

```sh
git clone https://github.com/ptitSeb/box64
cd box64 && mkdir build && cd build
cmake .. -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
# install, keeping the old one as a rollback:
sudo cp -a /usr/bin/box64 /usr/bin/box64-0.3.4   # backup
sudo cp ./box64 /usr/bin/box64
# rollback if needed: sudo cp -a /usr/bin/box64-0.3.4 /usr/bin/box64
```

What 0.4.3 fixes / changes here:
- **clone3 (435): FIXED** — threads spawn (0.3.4 hard-crashed).
- **~9% faster** than 0.3.4 on the sum-ms CPU bench.

## FEX is the binfmt default; box64 is explicit-invoke

box64 0.4.3 **cannot run static-glibc multithreaded x86-64 binaries** correctly: it
corrupts the glibc mutex `__owner` field (`Fatal glibc error: pthread_mutex_lock ...
assertion failed: mutex->__data.__owner == 0`), failing nondeterministically (SIGABRT /
SIGSEGV / deadlock). This was swept across **22 configurations (0 passes)** — strong
memory barriers, aligned atomics, safe flags, single-CPU, no-bigblock: **none** help.
It is a defect in box64's atomic/mutex emulation, not tunable away. (Dynamically-linked
MT x86-64 binaries run fine.)

The fix is to route bare `./x86_64binary` execs to **FEX**, which handles static *and*
dynamic MT correctly. This is done by an **empty `/etc/binfmt.d/box64.conf` override**:
per `binfmt.d(5)`, a same-named file in `/etc/` completely replaces the vendor
`/usr/lib/binfmt.d/box64.conf`, so systemd-binfmt registers **no** box64 rule and the
x86-64 ELF magic is matched only by `FEX-x86_64`.

```sh
# /etc/binfmt.d/box64.conf — comments only, no rule (disables box64's binfmt entry):
printf '# intentionally empty: route bare x86-64 ELF execs to FEX (box64 static-MT abort)\n' \
  | sudo tee /etc/binfmt.d/box64.conf
sudo systemctl restart systemd-binfmt.service
# verify: FEX-x86 + FEX-x86_64 registered, no box64 entry; cat /proc/sys/fs/binfmt_misc/box64 -> No such file
# rollback: sudo rm /etc/binfmt.d/box64.conf && sudo systemctl restart systemd-binfmt.service
```

This is persistent across reboot **and** survives box64 package upgrades (unlike editing
the vendor file or writing `/proc/.../box64`). `FEX-x86` (32-bit) and Hangover's wowbox64
are untouched. Wine/Steam/game launchers that call `box64` *explicitly* still get their
box64rc profiles — only bare execs change.

## box64rc tuning (`/etc/box64.box64rc`)

box64 is used explicitly for dynamically-linked games (lib-wrapping + per-game tuning).
Validated CPU-bench wins (paired A/B; see `../docs/BENCHMARKS.md`):

```
[BOX64]
BOX64_DYNAREC_CALLRET=1     # ~-9% (call/ret; helps indirect-call/qsort-heavy code)
BOX64_DYNAREC_SAFEFLAGS=0   # ~-4% extra, but a flag-correctness risk — validate per app
```

`CALLRET=1` alone is the safe ~-9%. **Do NOT** set `NATIVEFLAGS=0` (+35%) or raise
`STRONGMEM`/`BIGBLOCK` (slower; the defaults are already optimal here). No persistent
dynarec cache exists in 0.4.3.

## Notes specific to A733 / PowerVR
- For **GPU** workloads under box64, pair it with the native Vulkan ICD
  (`/usr/share/vulkan/icd.d/img_icd.json`) — Vulkan apps reach the real GPU.
- For **Direct3D**, the working path is **DXVK-Sarek under wine/Hangover** (see
  `../gpu/dxvk/`), not box64 directly.
- FEX vs box64: FEX is the correctness-default and the binfmt handler; box64's strength
  is dynamically-linked games via lib-wrapping + the tuned box64rc. Pick per app.
