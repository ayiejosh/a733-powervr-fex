#!/bin/bash
# Chrome (x86-64) under FEX on PowerVR BXM ARM board.
# Key fixes:
#   --in-process-gpu: eliminates GPU subprocess IPC dependency that blocked renderer creation
#   --ipc-connection-timeout=3600: Chrome startup under FEX takes ~3-5 min cold (JIT compile);
#     180s timeout fired before browser could connect to child processes
#   --disable-extensions + stripped startup: reduces JIT compilation work at startup
#   AOT cache (ENABLECODECACHINGWIP): pre-compiled 51741 blocks; cache must be generated
#     with FEX_ENABLECODECACHINGWIP=1 (in Config.json) so JIT uses NOP-padded LoadConstant
#     matching the fixed-width DOPAD applied at load time. Bug found 2026-06-11.
# Rendering confirmed stable (7/7 sites, 32+ min uptime).
export FEX_ROOTFS=/home/radxa/crd-rootfs
export DISPLAY=:0 XAUTHORITY=/home/radxa/.Xauthority
export HOME=/home/radxa/crd-rootfs/home/crd USER=crd XDG_RUNTIME_DIR=/tmp/fexrun
export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/1000/bus"
export LD_LIBRARY_PATH=/usr/local/lib
export FEX_THUNKHOSTLIBS=/home/radxa/FEX-src/build-native/HostLibs_64
export FEX_THUNKGUESTLIBS=/usr/share/fex-emu/GuestThunks
export FEX_THUNKCONFIG=/home/radxa/.fex-emu/ThunkConfig.json
export FEX_SERVERSOCKETPATH=/run/user/1000/1000.FEXServer.Socket
# AOT code cache DISABLED: FEXOfflineCompiler compiles 159930 blocks (follows branch targets
# beyond the 51744-block codemap input), filling multiple code buffers. SaveData saves only the
# last buffer but accumulates ALL relocations → 3.5M of 4.87M relocations have offsets past
# CodeBufferSize. LOGMAN_THROW_A_FMT is a no-op in release builds, so these corrupt offsets
# write past the staging buffer (non-lazy) or leave ExitFunctionLinker=0 (lazy) → SIGSEGV.
# Chrome is stable in JIT-only mode (slower cold start, ~3-5 min). LOCAL PATCH 2026-06-11.
export FEX_APP_CACHE_LOCATION=/home/radxa/.cache/fex-emu/
export FEX_ENABLECODECACHINGWIP=0
export FEX_ENABLELAZYCODECACHINGWIP=0
mkdir -p /home/radxa/.cache/fex-emu/cache
mkdir -p /tmp/fexrun; chmod 700 /tmp/fexrun
pgrep FEXServer >/dev/null || { /opt/fex/bin/FEXServer & disown; sleep 2; }

# SANDBOX STATUS (investigated 2026-06-11, Chrome 149.0.7827.102 under FEX):
#   --no-sandbox is REQUIRED, not laziness. Root cause is the ZYGOTE LAUNCH, not seccomp:
#   with ANY sandbox enabled Chrome dies at zygote_host_impl_linux.cc:221
#   "Check failed: . : No such file or directory (2)" — the browser's base::LaunchProcess of
#   the zygote (it execs /proc/self/exe --type=zygote) returns ENOENT under FEX's emulated
#   clone+exec handshake. This reproduces with EVERY layer-1 variant:
#     - default (setuid)                       -> zygote:221 ENOENT
#     - --disable-setuid-sandbox (userns)      -> zygote:221 ENOENT
#     - --disable-seccomp-filter-sandbox       -> zygote:221 ENOENT
#     - --disable-namespace+--disable-setuid   -> zygote:128 "No usable sandbox!" (Chrome refuses)
#     - --no-zygote (+sandbox)                 -> refused: "Zygote cannot be disabled if sandbox is enabled"
#   The primitives themselves DO work under FEX: unprivileged userns/pid/net clone via `unshare`
#   succeeds, and FEX has a full seccomp-bpf emulator (SeccompEmulator/BPFEmitter) — exporting
#   FEX_NEEDSSECCOMP=1 makes Chrome's "Lacking support for seccomp-bpf sandbox" probe PASS.
#   But Chrome's specific zygote-host process launch can't complete through FEX, and the zygote
#   is mandatory for any sandbox, so there is NO sandbox-enabled config that actually runs.
#   chrome-sandbox is correctly root:root mode 4755; user.max_user_namespaces>0. Neither is the issue.
#   => --no-sandbox stays. The flags below reduce attack surface independently of the sandbox.
#   Revisit if a FEX update fixes guest clone/exec for the zygote, or if Chrome gains an
#   emulator-friendly zygote launch path.
[ $# -eq 0 ] && set -- https://example.com

exec taskset -c 6-7 /opt/fex/bin/FEXInterpreter /home/radxa/crd-rootfs/opt/google/chrome/chrome \
  --no-sandbox --ozone-platform=x11 \
  --disable-dev-shm-usage \
  --disable-features=MediaRouter,OptimizationHints \
  --disable-component-update \
  --disable-domain-reliability \
  --disable-gpu --disable-gpu-compositing \
  --in-process-gpu \
  --ipc-connection-timeout=3600 \
  --no-first-run --no-default-browser-check \
  --disable-extensions \
  --disable-background-networking \
  --no-pings \
  --metrics-recording-only \
  --disable-default-apps \
  --disable-sync \
  --renderer-process-limit=2 \
  --remote-debugging-port=9222 --remote-allow-origins=* \
  --enable-logging=stderr --v=0 \
  --user-data-dir=/home/radxa/chrome-data \
  "$@"
