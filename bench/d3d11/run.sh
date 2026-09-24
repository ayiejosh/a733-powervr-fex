#!/usr/bin/env bash
# run.sh - the D3D11 half of the full benchmark, against the VENDOR GPU stack
# (no driver module swap: the vendor `pvrsrvkm` is what the desktop uses).
#
#   ./run.sh matrix     correctness matrix: every harness app, with expectations
#   ./run.sh timed      the timed suite behind docs/BENCHMARKS.md
#   ./run.sh gsab       geometry-shader emulation A/B (needs GS_DLL, swaps a dll)
#   ./run.sh all        matrix + timed
#
# Environment:
#   D3D_BIN       where the .exe harness lives   (default /home/radxa/gpu-experiment/mind3d)
#   LOGDIR        where logs go                  (default ./logs-<timestamp>)
#   SHIPPING_DLL  the deployed BCn d3d11.dll     (default /tmp/d3d11.shipping.dll)
#   GS_DLL        dll built from a `gs-*` branch (required by `gsab`)
#   QUICK=1       a three-app subset of the matrix, for a smoke run
#
# `gsab` is the only mode that writes to the wine prefix; it backs the current dll up
# first and restores it on exit, whatever happens.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
D3D_BIN="${D3D_BIN:-/home/radxa/gpu-experiment/mind3d}"
LOGDIR="${LOGDIR:-$HERE/logs-$(date +%Y%m%d-%H%M%S)}"
SHIPPING_DLL="${SHIPPING_DLL:-/tmp/d3d11.shipping.dll}"
GS_DLL="${GS_DLL:-}"
PREFIX_DLL="${PREFIX_DLL:-/home/radxa/.wine-dxvk/drive_c/windows/system32/d3d11.dll}"
PLAIN="${PLAIN:-/usr/local/bin/d3drun}"
DXVCONF="${DXVCONF:-/home/radxa/dxvk.conf}"
GSRUN=/tmp/gsrun.sh
GSCONF=/tmp/gsconf.conf
mkdir -p "$LOGDIR"

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
need_exe() { [ -x "$D3D_BIN/$1" ] || { say "missing $D3D_BIN/$1 - run ./build.sh first"; exit 2; }; }

# ── matrix ────────────────────────────────────────────────────────────────────
# app | output marker that proves it got all the way through | ok|known
MATRIX="
tri.exe|RENDERED_6000_FRAMES_OK|ok
cube.exe|CUBE_DONE|ok
tex.exe|TEXTURE_OK|ok
depth.exe|DEPTH_OK|ok
mrt2.exe|MRT_OK|ok
rtt.exe|RTT_OK|ok
compute.exe|COMPUTE_OK|ok
cgs.exe|COMPUTE_GS_ARCH_OK|ok
cgs2.exe|DYN_AMPLIFY_OK|ok
bctex.exe|BC_TRANSCODE_OK|ok
bc2t.exe|BC2_OK|ok
bc4t.exe|BC4_OK|ok
bc5t.exe|BC5_OK|ok
bcbench.exe|BCBENCH_OK|ok
bcdxvk.exe|BC1_DXVK_OK|ok
bcdxvk3.exe|BC3_DXVK_OK|ok
min_d3d11.exe|PRESENTED_600_FRAMES_OK|ok
bench.exe|RESULT|ok
bench2.exe|RESULT|ok
bench_flip.exe|RESULT|ok
drawbench.exe|RESULT|ok
mrt.exe|MRT_FAIL|known
msaa.exe|MSAA_FAIL|known
msaa2.exe|MSAA_FAIL|known
tess.exe|tess_rc|known
d7test.exe|d7_rc|known
d3d7test.exe|d7_rc|known
"
# Why the five `known` rows: the BXM blob has no MSAA and no tessellation, and there is
# no D3DHALDevice for D3D7; `mrt.exe` is the SV_VertexID/no-input-layout variant (mrt2
# with a real vertex buffer is MRT_OK). All five fail identically with the session
# Vulkan layer/fakes removed, i.e. they are not ours. See docs/FULL-BENCHMARK.md.

do_matrix() {
    say "=== matrix (vendor stack, $D3D_BIN) ==="
    printf '%-16s %-5s %-7s %-8s %s\n' app rc secs verdict marker | tee "$LOGDIR/matrix.txt"
    ok=0; known=0; bad=0
    for row in $MATRIX; do
        app=${row%%|*}; rest=${row#*|}; marker=${rest%%|*}; expect=${rest##*|}
        [ "${QUICK:-}" = 1 ] && case "$app" in tri.exe|compute.exe|bctex.exe) ;; *) continue ;; esac
        need_exe "$app"
        t0=$(date +%s.%N)
        ( cd "$D3D_BIN" && timeout 400 "$PLAIN" "./$app" ) > "$LOGDIR/$app.log" 2>&1
        rc=$?
        secs=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
        got=$(grep -aE "RESULT|_OK|_FAIL|center RGB" "$LOGDIR/$app.log" | tail -1 | cut -c1-60)
        case "$marker" in
            tess_rc|d7_rc) pass=$([ $rc -eq 0 ] && echo 1 || echo 0) ;;
            *)             pass=$([ $rc -eq 0 ] && grep -qa "$marker" "$LOGDIR/$app.log" && echo 1 || echo 0) ;;
        esac
        if [ "$pass" = 1 ] && [ "$expect" = ok ]; then
            verdict=clean; ok=$((ok+1))
        elif [ "$pass" = 0 ] && [ "$expect" = known ]; then
            verdict=known-open; known=$((known+1))
        elif [ "$pass" = 1 ]; then
            verdict="clean (known row now passes)"; ok=$((ok+1))
        else
            verdict="UNEXPECTED FAIL"; bad=$((bad+1))
        fi
        printf '%-16s %-5s %-7s %-8s %s\n' "$app" "$rc" "$secs" "$verdict" "$got" | tee -a "$LOGDIR/matrix.txt"
    done
    say "matrix: $ok clean, $known known-open, $bad unexpected"
    [ "$bad" -eq 0 ]
}

# ── timed ─────────────────────────────────────────────────────────────────────
timed_one() {
    local label="$1"; shift
    ( cd "$D3D_BIN" && timeout 900 "$PLAIN" "$@" ) > "$LOGDIR/timed-$label.log" 2>&1
    local rc=$?
    printf '  %-16s rc=%-4s %s\n' "$label" "$rc" \
        "$(grep -aE "RESULT|totalMs|msPerFrame" "$LOGDIR/timed-$label.log" | tail -1 | cut -c1-88)"
}

do_timed() {
    say "=== timed suite (vendor stack) ==="
    need_exe drawbench.exe; need_exe realbench.exe; need_exe bench.exe
    if [ "${QUICK:-}" = 1 ]; then
        timed_one "drawbench-a" drawbench.exe a 2000 256
        timed_one "bcbench-1"   bcbench.exe
        say "QUICK=1: the rest of the timed suite was skipped"
        return 0
    fi
    for c in a b c d e f g; do timed_one "drawbench-$c" drawbench.exe "$c" 2000 256; done
    timed_one "drawbench-a2" drawbench.exe a 2000 256          # same-session repeat
    timed_one "realbench-r"  realbench.exe r 200 512 500 1000 2000 4000
    timed_one "realbench-p"  realbench.exe p 300 800 600
    timed_one "bench-1draw"  bench.exe 1 1 8
    timed_one "bench-256"    bench.exe 256 1 8
    timed_one "bench2-inst"  bench2.exe 1000 1 6
    timed_one "benchflip-256" bench_flip.exe 256 1 8
    timed_one "bcbench-1"    bcbench.exe
    timed_one "bcbench-2"    bcbench.exe
    say "raw output: $LOGDIR/timed-*.log"
}

# ── gs A/B ────────────────────────────────────────────────────────────────────
gs_run() {   # gs_run <label> <dll> <wrapper> <timeout> <app> [args...]
    local label="$1" dll="$2" wrapper="$3" tmo="$4"; shift 4
    cp -f "$dll" "$PREFIX_DLL"
    ( cd "$D3D_BIN" && timeout "$tmo" "$wrapper" "./$@" ) > "$LOGDIR/gs-$label.log" 2>&1
    local rc=$?
    printf '  %-18s rc=%-4s %s\n' "$label" "$rc" \
        "$(grep -aE "RESULT|GS_OK|GS_FAIL|center RGB|CUBE_DONE" "$LOGDIR/gs-$label.log" | tail -1 | cut -c1-78)"
}

do_gsab() {
    [ -n "$GS_DLL" ] || { say "gsab needs GS_DLL=<dll built from a gs-* branch>"; return 2; }
    [ -f "$GS_DLL" ] || { say "GS_DLL=$GS_DLL not found"; return 2; }
    [ -f "$SHIPPING_DLL" ] || { say "SHIPPING_DLL=$SHIPPING_DLL not found (keep a copy of the deployed BCn dll)"; return 2; }
    sed 's/^d3d11\.emulateGeometryShaders.*/d3d11.emulateGeometryShaders = True/' "$DXVCONF" > "$GSCONF"
    grep -q '^d3d11\.emulateGeometryShaders' "$GSCONF" || echo 'd3d11.emulateGeometryShaders = True' >> "$GSCONF"
    if [ ! -x "$GSRUN" ]; then
        sed "s|DXVK_CONFIG_FILE=$DXVCONF|DXVK_CONFIG_FILE=$GSCONF|" "$PLAIN" > "$GSRUN"
        chmod +x "$GSRUN"
    fi
    cp -f "$PREFIX_DLL" "$LOGDIR/d3d11.dll.before"
    trap 'cp -f "$SHIPPING_DLL" "$PREFIX_DLL"; say "restored deployed dll"' EXIT
    say "=== GS A/B (gsdll=$(md5sum "$GS_DLL" | cut -c1-8), deployed=$(md5sum "$SHIPPING_DLL" | cut -c1-8)) ==="
    gs_run "gate"        "$GS_DLL"      "$GSRUN" 300 gs.exe
    gs_run "nogs"        "$SHIPPING_DLL" "$PLAIN" 600 gsbench.exe nogs 2000 64
    gs_run "nogs-gsdll"  "$GS_DLL"      "$PLAIN" 600 gsbench.exe nogs 2000 64
    gs_run "gs-1"        "$GS_DLL"      "$GSRUN" 900 gsbench.exe gs 2000 64
    gs_run "gs-2"        "$GS_DLL"      "$GSRUN" 900 gsbench.exe gs 2000 64
    gs_run "bc-texture"  "$GS_DLL"      "$PLAIN" 400 cube.exe
    say "raw output: $LOGDIR/gs-*.log"
}

for m in "$@"; do
    case "$m" in
        matrix) do_matrix ;;
        timed)  do_timed ;;
        gsab)   do_gsab ;;
        all)    do_matrix && do_timed ;;
        *)      echo "usage: $0 [matrix|timed|gsab|all]" >&2; exit 2 ;;
    esac || exit $?
done
[ $# -gt 0 ] || { echo "usage: $0 [matrix|timed|gsab|all]" >&2; exit 2; }
say "logs in $LOGDIR"
