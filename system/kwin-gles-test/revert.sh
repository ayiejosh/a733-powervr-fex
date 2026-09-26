#!/bin/bash
# revert.sh — put the desktop back on the known-good compositor configuration:
#   KWin's own compositing OFF (kwinrc [Compositing] Enabled=false) + picom --backend xrender.
#
# Idempotent. Runs either from the agent shell (as radxa, no sudo needed) or from
# gpu-test-guard.service at boot (as root, via runuser). The guard calls it only while
# the IN-PROGRESS sentinel exists, so a kernel hang mid-test cannot leave a boot loop.
set -u
H=/home/radxa; D=$H/gpu-desktop-test; KR=$H/.config/kwinrc
U=$(stat -c %U "$H")
RUNAS=""
[ "$(id -u)" = 0 ] && RUNAS="runuser -u $U --"
log(){ echo "[revert $(date +%H:%M:%S)] $*"; }

# 1. kwinrc: compositing off
if command -v kwriteconfig6 >/dev/null 2>&1; then
  $RUNAS kwriteconfig6 --file kwinrc --group Compositing --key Enabled false && log "kwinrc Enabled=false (kwriteconfig6)"
elif [ -f "$D/kwinrc.bak-gles-test" ]; then
  cp -f "$D/kwinrc.bak-gles-test" "$KR"; log "kwinrc restored from backup"
else
  python3 - "$KR" <<'PY'
import re,sys
p=sys.argv[1]; s=open(p).read()
s2,n=re.subn(r'(\[Compositing\]\s*\n(?:[^\[]*?))Enabled=\w+', r'\1Enabled=false', s, count=1)
open(p,'w').write(s2 if n else s)
print("edited" if n else "no [Compositing] Enabled key found")
PY
fi
rm -f "$D/IN-PROGRESS"

# 1b. remove the KWin OpenGL-override drop-in — the other half of the test, and the part that
#     matters most for boot safety (with it gone, a reboot cannot re-enter GL compositing).
DROPIN=$H/.config/systemd/user/plasma-kwin_x11.service.d/gles-test.conf
if [ -f "$DROPIN" ]; then rm -f "$DROPIN"; log "removed $DROPIN"; fi

# 2. picom back (the XRender compositor the board uses instead of KWin's GL)
if ! pgrep -x picom >/dev/null 2>&1; then
  $RUNAS env DISPLAY=:0 XAUTHORITY=$H/.crd-xauth XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    setsid nohup picom --backend xrender --daemon >/dev/null 2>&1 &
  log "picom started"
else
  log "picom already running"
fi

# 3. KWin back through its own unit (supervised — a bare `kwin_x11 --replace` from a script
#    gets killed when that script's session tears down, which is how this was learned)
sleep 1
if pgrep -x kwin_x11 >/dev/null 2>&1 || systemctl --user is-active plasma-kwin_x11.service >/dev/null 2>&1; then
  $RUNAS env XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    systemctl --user daemon-reload
  $RUNAS env XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    systemctl --user restart plasma-kwin_x11.service && log "plasma-kwin_x11.service restarted (software env, compositing off)"
else
  log "kwin_x11 not running - nothing to restart"
fi
log "done: KWin compositing OFF, picom XRender active"
