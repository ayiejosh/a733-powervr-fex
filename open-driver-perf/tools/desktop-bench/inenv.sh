#!/bin/bash
# inenv.sh CMD... - run CMD with the environment of the running Plasma session (plasmashell's), as an
# app started from the desktop gets it. Used while daily.sh holds a desktop up (HOLD=1).
p=$(pgrep -u radxa -x plasmashell | head -1)
[ -n "$p" ] || { echo "inenv: no plasmashell running" >&2; exit 1; }
mapfile -d '' e < /proc/$p/environ
exec env -i "${e[@]}" "$@"
