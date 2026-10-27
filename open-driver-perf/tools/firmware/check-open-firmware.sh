#!/bin/sh
# Check where this board's open-ABI firmware comes from and whether IMG published a newer build.
# The official source is Imagination's own linux-firmware fork, branch "powervr".
FW=rogue_36.56.104.183_v1.fw
GL=https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/powervr/powervr/$FW
GAPI="https://gitlab.freedesktop.org/api/v4/projects/imagination%2Flinux-firmware/repository/commits?path=powervr&ref_name=powervr&per_page=3"
LOCAL=/lib/firmware/powervr/$FW
echo "== board firmware =="
[ -f "$LOCAL" ] && sha256sum "$LOCAL" | cut -d' ' -f1
echo "== IMG powervr branch, latest commits =="
curl -sL "$GAPI" | python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(0)
for c in d: print(c["short_id"], c["committed_date"][:10], c["title"][:70])' 2>/dev/null
echo "== upstream file in IMG branch =="
if curl -sfL -o /tmp/img-fw-check.fw "$GL"; then
   h=$(sha256sum /tmp/img-fw-check.fw | cut -d' ' -f1)
   echo "$FW sha256: $h"
   if [ -f "$LOCAL" ] && [ "$h" = "$(sha256sum "$LOCAL" | cut -d' ' -f1)" ]; then
      echo "  same as board (official IMG build; no update needed)"
   else
      echo "  !! differs from board - test the IMG branch build"
   fi
else
   echo "  download failed"
fi
