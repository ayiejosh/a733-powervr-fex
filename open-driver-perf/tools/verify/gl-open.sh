#!/bin/sh
# Run an EGL/GLES harness on the OPEN stack without ever starting it with the vendor driver bound
# (that wedged the GPU and caused watchdog resets twice on 2026-10-29).
/home/radxa/Desktop/Projects/Radxa-A7A/open-driver-perf/tools/verify/switch-safe.sh open || exit 1
exec "$@"
