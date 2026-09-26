#!/usr/bin/env bash
# Cross-build the D3D11 harness (x86-64 PE, run through the Windows + DXVK stack).
#
#   ./build.sh                      # -> /home/radxa/gpu-experiment/mind3d
#   OUT=/tmp/d3dbin ./build.sh      # elsewhere
#
# The harness is x86-64 Windows code on purpose: it takes the same path an application
# takes (wine -> DXVK-Sarek arm64ec -> PowerVR Vulkan), not a native arm64 shortcut.
#
# Requires the mingw-w64 cross toolchain (x86_64-w64-mingw32-clang++ from clang, or
# x86_64-w64-mingw32-g++ from gcc-mingw-w64). Nothing else.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-/home/radxa/gpu-experiment/mind3d}"
CXX="${CXX:-x86_64-w64-mingw32-clang++}"
BASE="-O2 -ld3d11 -ld3dcompiler"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "build.sh: $CXX not found" >&2
    echo "  The harness needs the mingw-w64 cross toolchain:" >&2
    echo "      sudo apt-get install -y mingw-w64" >&2
    echo "  or point CXX at another x86_64-w64-mingw32 compiler." >&2
    exit 2
fi

mkdir -p "$OUT"
fail=0
for src in "$HERE"/src/*.cpp; do
    name=$(basename "$src" .cpp)
    extra=""
    case "$name" in
        # the two D3D7 probes want the old DirectDraw/D3D7 interfaces as well
        d7test|d3d7test) extra="-ldxguid -lddraw" ;;
    esac
    if $CXX $BASE $extra "$src" -o "$OUT/$name.exe" 2>"$OUT/$name.build.log"; then
        printf '  %-18s ok\n' "$name.exe"
        rm -f "$OUT/$name.build.log"
    else
        printf '  %-18s FAILED (see %s)\n' "$name.exe" "$OUT/$name.build.log"
        fail=$((fail + 1))
    fi
done

echo "build.sh: $(( $(ls -1 "$OUT"/*.exe 2>/dev/null | wc -l) )) exe(s) in $OUT, $fail build failure(s)"
[ "$fail" -eq 0 ]
