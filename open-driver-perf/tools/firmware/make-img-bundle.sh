#!/bin/sh
# Build img-evidence-bundle.tar.gz for the IMG firmware/guidance request (request text, analysis docs,
# kick-time evidence and benchmark logs). Run from open-driver-perf/.
set -e
cd "$(dirname "$0")/../.."
B=$(mktemp -d)
mkdir -p "$B/img-bundle"
cp docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md TA-KICK-DISASSEMBLY.md RESULTS.md PERF-PARITY-STATUS.md "$B/img-bundle/"
cp -r captures/kickregs captures/bench-2026-10-27 "$B/img-bundle/" 2>/dev/null || true
tar -C "$B" -czf img-evidence-bundle.tar.gz img-bundle
rm -rf "$B"
ls -la img-evidence-bundle.tar.gz
echo "attach this file to the email / issue"
