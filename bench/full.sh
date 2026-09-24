#!/usr/bin/env bash
# full.sh - one entry point for the whole benchmark, so "is anything broken?" is a
# single command and every number has the same shape of evidence behind it.
#
#   ./full.sh                     # the two always-safe phases (canonical + d3d)
#   ./full.sh canonical d3d       # the same, spelled out
#   ./full.sh open                # adds the open-driver suite (MODULE SWAP, desktop blips)
#   ./full.sh gs                  # geometry-shader emulation A/B (swaps a dll; needs GS_DLL)
#   ./full.sh all                 # everything, including open and (if GS_DLL is set) gs
#   ./full.sh --list              # what each phase covers and what it needs
#
# Phases:
#   canonical  bench/run.sh                     CPU, FEX/x87, GPU GLES throughput     ~3 min
#   d3d        bench/d3d11/run.sh all           D3D11 matrix + timed suite            ~20 min
#   gs         bench/d3d11/run.sh gsab          GS emulation A/B (opt-in dll)         ~6 min
#   open       bench/pvr-vulkan/regress.sh      Vulkan + GL over the open driver      ~2 min
#              (through kernel/open-driver-spike/open-run.sh, which unloads the vendor
#               module, runs the suite and restores the desktop)
#
# Environment:
#   LOGDIR        default ./logs-<timestamp> (each phase keeps its own logs there)
#   GS_DLL        dll built from a `gs-*` branch, required by the gs phase
#   QUICK=1       three-app smoke matrix instead of the full one
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LOGDIR="${LOGDIR:-$HERE/logs-$(date +%Y%m%d-%H%M%S)}"
OPEN_RUN="${OPEN_RUN:-$ROOT/kernel/open-driver-spike/open-run.sh}"

if [ "${1:-}" = "--list" ]; then
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

mkdir -p "$LOGDIR"
export LOGDIR


say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

phases=("$@")
[ ${#phases[@]} -eq 0 ] && phases=(canonical d3d)

declare -a failed=()
run_phase() {
    local name="$1"; shift
    say "### phase $name: $*"
    if "$@" 2>&1 | tee "$LOGDIR/$name.txt"; then
        say "### phase $name: ok  ($LOGDIR/$name.txt)"
    else
        say "### phase $name: FAILED  ($LOGDIR/$name.txt)"
        failed+=("$name")
    fi
}

for p in "${phases[@]}"; do
    case "$p" in
        canonical) run_phase canonical "$HERE/run.sh" ;;
        d3d)       run_phase d3d "$HERE/d3d11/run.sh" all ;;
        gs)        run_phase gs "$HERE/d3d11/run.sh" gsab ;;
        open)
            [ -x "$OPEN_RUN" ] || { say "open: $OPEN_RUN is not executable"; failed+=("open"); continue; }
            say "### phase open: desktop goes down and comes back - do not touch the board"
            # The desktop GL change exports zink + the feature-strip layer session-wide.
            # regress.sh must not inherit them: the strip layer would fake/strip features
            # on the open driver too and the depthClamp/vk13 cases would stop testing what
            # they say they test (measured: 29/29 clean with them stripped).
            run_phase open env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u PVR_FAKE_GS \
                -u PVR_FAKE_FILL -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
                -u LIBGL_DRIVERS_PATH "$OPEN_RUN" "$HERE/pvr-vulkan/regress.sh"
            ;;
        all)
            for q in canonical d3d open; do
                case "$q" in
                    canonical) run_phase canonical "$HERE/run.sh" ;;
                    d3d)       run_phase d3d "$HERE/d3d11/run.sh" all ;;
                    open)
                        [ -x "$OPEN_RUN" ] || { say "open: $OPEN_RUN is not executable"; failed+=("open"); continue; }
                        # The desktop GL change exports zink + the feature-strip layer session-wide.
            # regress.sh must not inherit them: the strip layer would fake/strip features
            # on the open driver too and the depthClamp/vk13 cases would stop testing what
            # they say they test (measured: 29/29 clean with them stripped).
            run_phase open env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u PVR_FAKE_GS \
                -u PVR_FAKE_FILL -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
                -u LIBGL_DRIVERS_PATH "$OPEN_RUN" "$HERE/pvr-vulkan/regress.sh"
                        ;;
                esac
            done
            if [ -n "${GS_DLL:-}" ]; then run_phase gs "$HERE/d3d11/run.sh" gsab
            else say "### phase gs: skipped (set GS_DLL to run it)"; fi
            ;;
        *) echo "full.sh: unknown phase '$p' (try --list)" >&2; exit 2 ;;
    esac
done

{
    echo "full.sh $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "phases: ${phases[*]}"
    echo "logs:   $LOGDIR"
    echo "failed: ${failed[*]:-none}"
    for f in canonical d3d gs open; do
        [ -f "$LOGDIR/$f.txt" ] || continue
        echo
        echo "== $f: last lines =="
        tail -6 "$LOGDIR/$f.txt"
    done
} | tee "$LOGDIR/SUMMARY.txt"

[ ${#failed[@]} -eq 0 ] || { say "full.sh: ${#failed[@]} phase(s) failed: ${failed[*]}"; exit 1; }
say "full.sh: all phases ok"
