#!/bin/sh
# run-all.sh: the native driver's whole verify gate in one go, on the open stack.
# Exits non-zero on the first failing group; prints one line per group.
# Run with the open driver bound (switch-safe.sh open) and the env26.sh sourced.
set -u
OP=/home/radxa/open-perf
V=/home/radxa/a733-powervr-fex/open-driver-perf/tools/verify
OUT=/tmp/run-all; mkdir -p $OUT
export MESA_LOADER_DRIVER_OVERRIDE=pvr MESA_SHADER_CACHE_DISABLE=1
fail=0
ok() { printf 'PASS %s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

# Compile the probes from source so the gate is self-contained.
for p in cube_probe cube_mip border_probe mirror_probe aniso_probe polygon_offset_probe rg_probe f16rt_probe; do
   [ -f "$V/$p.c" ] || continue
   gcc -O2 "$V/$p.c" -o "$OUT/$p" -lEGL -lGLESv2 || bad "compile $p"
done

sets=$(sh $OP/tri.sh 2>&1 | tee $OUT/tri.log | grep -c '^== ')
errs=$(grep -c 'glGetError: 0x0' $OUT/tri.log)
nz=$(grep -c 'glGetError: 0x[^0]' $OUT/tri.log)
[ "$nz" = 0 ] && [ "$errs" -ge 32 ] && ok "gltri $sets modes / $errs clean" || bad "gltri (nz=$nz ok=$errs)"

mip=$(sh $OP/tri.sh mip 2>&1 | tee $OUT/mip.log | grep -c 'pixel inside')
mnz=$(grep -c 'glGetError: 0x[^0]' $OUT/mip.log)
[ "$mnz" = 0 ] && [ "$mip" -ge 14 ] && ok "mip $mip levels" || bad "mip (nz=$mnz ok=$mip)"

r=$(timeout 120 $OP/rttest 2>&1 | tail -2 | tee $OUT/rt.log)
echo "$r" | grep -q '^PASS' && ok "rttest" || bad "rttest: $(echo "$r" | tr '\n' ' ')"
[ "$(timeout 120 $OP/rt8test 2>&1 | grep -c 'readback')" -ge 2 ] && ok "rt8test" || bad "rt8test"
[ "$(timeout 120 $OP/mrt_test 2>&1 | grep -c 'attachment')" -ge 2 ] && ok "mrt 2 targets" || bad "mrt"
[ "$(MRT_ATT=4 timeout 120 $OP/mrt_test 2>&1 | grep -c 'attachment')" -ge 4 ] && ok "mrt 4 targets" || bad "mrt 4"
[ "$(timeout 120 $OP/inst_test 2>&1 | grep -c 'instance[0-9]')" -ge 3 ] && ok "instancing" || bad "instancing"
[ "$(timeout 120 $OP/srgb_test 2>&1 | grep -c 'err 0x0')" -ge 3 ] && ok "sRGB" || bad "sRGB"
[ "$(timeout 120 $OP/ext_texture_test 2>&1 | grep -c 'err 0x0')" -ge 3 ] && ok "extended textures" || bad "extended textures"
[ "$(timeout 120 $OP/f16lin_test 2>&1 | grep -c 'err 0x0')" -ge 2 ] && ok "half-float linear" || bad "half-float linear"

[ "$(timeout 120 $OUT/cube_probe 2>&1 | grep -c 'err 0x0')" -ge 6 ] && ok "cube faces" || bad "cube faces"
[ "$(timeout 120 $OUT/cube_mip 2>&1 | grep -c 'err 0x0')" -ge 6 ] && ok "cube mips" || bad "cube mips"
[ "$(timeout 120 $OUT/border_probe 2>&1 | grep -c '0 255 0 255')" -ge 1 ] && ok "border clamp" || bad "border clamp"
[ "$(timeout 120 $OUT/mirror_probe 2>&1 | grep -c '200 0 0 255')" -ge 1 ] && ok "mirror clamp" || bad "mirror clamp"
[ "$(timeout 120 $OUT/aniso_probe 2>&1 | grep -c 'maxaniso=16')" -ge 1 ] && ok "anisotropy" || bad "anisotropy"
po=$(timeout 120 $OUT/polygon_offset_probe 2>&1)
echo "$po" | grep -q 'C less off +40000   : 0 0 0 255' && ok "polygon offset" || bad "polygon offset"

fr=$(timeout 120 $OUT/f16rt_probe 2>&1)
echo "$fr" | grep -q 'RESULT: PASS' && ok "half-float colour targets" || bad "half-float colour targets"

if [ $fail = 0 ]; then echo "ALL GROUPS PASS"; else echo "SOME GROUPS FAILED"; fi
exit $fail
