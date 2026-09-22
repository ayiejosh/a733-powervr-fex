#!/bin/bash
# box64-rcfile-salvage.sh — is the rcfile hang about content, or about permissions?
#
# Established with a trustworthy purge:
#   rcfile absent          -> 1.0 s, ok, 3/3
#   rcfile present (any)   -> hang, 3/3 + 7/7 including an empty [*]
#
# And the original "proof" that this path is read was invalid: it was
# `timeout 90 wine ... | grep -c BOX64`, which prints 0 both when the banner is
# suppressed AND when the run is killed before printing anything. The 0 was a
# hang, not a suppressed banner.
#
# So the question this answers is narrow and practical: can the file be made to
# work at all (mode? location?), or must the prefix stay without one?
set -u
export WINEPREFIX=/home/radxa/.wine-dxvk WINEDEBUG=-all DISPLAY=
R=$WINEPREFIX/drive_c/users/radxa/.box64rc
CMD='C:\windows\syswow64\cmd.exe'
TUNED='[*]
BOX64_DYNAREC_SAFEFLAGS=0
BOX64_DYNAREC_BIGBLOCK=2
BOX64_DYNAREC_FORWARD=512
'
PROCS="wine wine64 wineserver services.exe winedevice.exe plugplay.exe explorer.exe cmd.exe conhost.exe rpcss.exe svchost.exe start.exe winemenubuilder.exe"
alive() { ps -eo comm 2>/dev/null | grep -cE '^(wineserver|services\.exe|winedevice\.exe|plugplay\.exe|cmd\.exe)$' || true; }
purge() {
  wineserver -k 2>/dev/null || true; sleep 1
  for p in $PROCS; do pkill -x "$p" 2>/dev/null || true; done; sleep 1
  for p in $PROCS; do pkill -9 -x "$p" 2>/dev/null || true; done
  for _ in $(seq 1 20); do [ "$(alive)" = "0" ] && break; pkill -9 -x winedevice.exe 2>/dev/null || true; sleep 0.5; done
}

trial() { # trial <label>
  local label=$1 s e rc
  purge
  s=$(date +%s.%N)
  timeout 25 env HODLL=wowbox64.dll wine "$CMD" /c ver >/dev/null 2>&1
  rc=$?
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" -v r="$rc" -v l="$label" \
    'BEGIN{printf "%-30s %7.2f s  exit=%-3d %s\n", l, b-a, r, (r==0?"ok":(r==124?"HANG":"fail"))}'
}

rm -f "$R"; trial "no rcfile (reference)"
printf '%s' "$TUNED" > "$R"; chmod 644 "$R"; trial "tuned, mode 644"
chmod 600 "$R"; trial "tuned, mode 600"
rm -f "$R"; chmod 644 "$R" 2>/dev/null; trial "absent again (recovery check)"

# The prefix must be left without this file, which is the working state.
rm -f "$R" "$R.full"
echo "left in place: $(ls -A $WINEPREFIX/drive_c/users/radxa/ | grep -c box64rc) box64rc files (want 0)"
