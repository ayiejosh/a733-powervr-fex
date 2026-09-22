# KWin-on-GPU test — tooling and the guard that makes it safe to try

The scripts that ran the "GPU-composited desktop" experiment on 2026-09-22, plus the guard that
makes it survivable. Result and analysis: [`../../docs/GPU-RESEARCH-2026-09-22.md`](../../docs/GPU-RESEARCH-2026-09-22.md) §7.

| file | what it is |
|---|---|
| `test.sh` | turns KWin's own compositing on and restarts `plasma-kwin_x11.service` with `KWIN_COMPOSE=O2ES` + `KWIN_OPENGL_INTERFACE=egl`, then reports what KWin chose and what the GPU did |
| `revert.sh` | back to the known-good desktop: `[Compositing] Enabled=false`, drop-in removed, picom XRender running, KWin restarted through its unit. `--config-only` fixes just the config (used at boot) |
| `gpu-test-guard.service` | install to `/etc/systemd/system/`, `systemctl enable`. `Before=display-manager`, acts only while a sentinel file exists, so a mid-test hang cannot become a boot loop |

How it is put together (the three things that are easy to get wrong on this board):

1. **Compositing is owned by KWin's unit, not by a loose process.** `systemctl --user restart
   plasma-kwin_x11.service` is the only restart that survives; a bare `kwin_x11 --replace` from a
   script gets killed when that script's session tears down.
2. **`UnsetEnvironment=LIBGL_ALWAYS_SOFTWARE QT_QUICK_BACKEND` is mandatory.**
   `~/.config/plasma-workspace/env/10-software-render.sh` exports software GL into the systemd user
   manager, so without unsets the "GPU" test silently measures llvmpipe. Verify with
   `tr '\0' '\n' < /proc/$(pgrep -x kwin_x11)/environ | grep -E 'KWIN_COMPOSE|LIBGL_ALWAYS'`.
3. **KWin remembers GL crashes.** After a failure it records `[Compositing] LastFailureTimestamp`
   and refuses OpenGL compositing ("video driver seems unstable") until it is cleared:
   `kwriteconfig6 --file kwinrc --group Compositing --key LastFailureTimestamp --delete`.

`system/kwin-gles-test` is inert by default: the drop-in and the sentinel only exist while `test.sh`
runs, and `revert.sh` removes both.
