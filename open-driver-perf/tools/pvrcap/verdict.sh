#!/bin/sh
# verdict.sh: outcome of the most recent TA kick in the firmware trace (PASS = TA finished, HANG = locked up)
sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0 | grep -a -E "Kick TA|TA finished|DM2 has locked up" | tail -2 | awk "/Kick TA/{k=1;next} k&&/TA finished/{print \"PASS\";exit} k&&/locked up/{print \"HANG\";exit} END{if(!k)print \"?\"}"
