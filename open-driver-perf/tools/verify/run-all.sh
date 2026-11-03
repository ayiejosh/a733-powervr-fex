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
t0=$(date '+%Y-%m-%d %H:%M:%S')
ok() { printf 'PASS %s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

# Compile the probes from source so the gate is self-contained.
for p in cube_probe cube_mip border_probe mirror_probe aniso_probe polygon_offset_probe rg_probe f16rt_probe zls_mip_probe scissor_probe clear_diag fragcoord_diag texexact_probe rgbx_probe discard_probe; do
   rm -f "$OUT/$p"
   [ -f "$V/$p.c" ] || { bad "missing $p.c"; continue; }
   gcc -O2 "$V/$p.c" -o "$OUT/$p" -lEGL -lGLESv2 || bad "compile $p"
done

sets=$(sh $OP/tri.sh 2>&1 | tee $OUT/tri.log | grep -c '^== ')
errs=$(grep -c 'glGetError: 0x0' $OUT/tri.log)
nz=$(grep -c 'glGetError: 0x[^0]' $OUT/tri.log)
[ "$nz" = 0 ] && [ "$errs" -ge 32 ] && ok "gltri $sets modes / $errs clean" || bad "gltri (nz=$nz ok=$errs)"
# The one gate case that writes (glTexSubImage) into a level a render compressed with TFBC: since 0390
# client-filled textures stay uncompressed, so only rendered levels reach the decode/patch/encode path.
grep -q 'subimg patch (5,5) 00 00 ff  triangle (256,200) ff 00 00' $OUT/tri.log && ok "gltri subimg into a rendered TFBC level" || bad "gltri subimg"

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

# Per-render state decided at flush time (review fixes on 0326/0367/0368/0370):
# A depth store past a depth-off last draw, B/F depth-clear store/handoff, C/D mips after an
# open or clear-only render, G mips while a depth-only FBO is bound.  E (batch RT data) passes
# everywhere; a case only counts once it fails on a build without the fix.
zr=$(timeout 120 $OUT/zls_mip_probe 2>&1)
if echo "$zr" | grep -q 'RESULT: PASS'; then
   ok "zls/mip per-render state (A B F C D G)"
else
   bad "zls/mip per-render state: $(echo "$zr" | grep -E 'FAIL' | tr '\n' ' ')"
fi

# Draw scissor (0379): S1 box edges off the tile grid, S2 scissor off after on, S3 per-draw index in one
# tile, S4 empty boxes, S6 boxes st clamped, S8 more boxes than the 64-entry table; S1 S3 S4 S6 S8 fail
# without 0379.  S5 (scissored glClear) and S7 (colour-masked glClear) go through st's clear quad; they
# failed until the vertex shader learned the bound vertex element formats (a vec4 attribute fed 3
# components must get w = 1, not the stale fourth register).  All cases must pass now.
sr=$(timeout 60 $OUT/scissor_probe 2>&1)
if echo "$sr" | grep -q '^RESULT: PASS'; then
   ok "draw scissor (S1-S8)"
else
   bad "draw scissor: $(echo "$sr" | grep '^FAIL' | head -5 | tr '\n' ' ')"
fi

# The clear quad itself (st draws a scissored or colour-masked glClear with a quad): D1-D4 scissored and
# colour-masked clears, D9 scissored clear after a draw, D17 a 3-component attribute right after a
# w = 2 draw, D18/D19 scissored clear right after a draw.
cr=$(timeout 120 $OUT/clear_diag 2>&1)
if echo "$cr" | grep -q '^DIAG-END' && ! echo "$cr" | grep -q '^DIAG BAD'; then
   ok "clear quad (D1-D19)"
else
   bad "clear quad: $(echo "$cr" | grep '^DIAG BAD' | head -5 | tr '\n' ' ')"
fi

# Fragment coordinates (0388): F1/F2 read gl_FragCoord directly, F3 the upload/readback round trip, F4
# sampling at centres built from floor(gl_FragCoord), F5 F2 on a pbuffer (drawn y-flipped); F1, F2 and
# F4 fail without 0388 (a half-pixel shift).
fc=$(timeout 60 $OUT/fragcoord_diag 2>&1)
if echo "$fc" | grep -q '^DIAG-END' && ! echo "$fc" | grep -q '^DIAG BAD'; then
   ok "fragment coordinates (F1-F5)"
else
   bad "fragment coordinates: $(echo "$fc" | grep '^DIAG BAD' | head -5 | tr '\n' ' ')"
fi

# Texel-exact sampling at 12 sizes in GL_RGBA and GL_RGB (all but 62x62 fail without 0388, odd widths
# also without 0391), odd-width render readback and partial upload (fail without 0391), a texture
# sampled before its first upload (exercises 0390's flag flip), MRT attachment 1 read back or written
# right after its render (need 0389's attachment tracking), glReadPixels from a GL_RGB target, R8/RG8
# gradients (fail without 0393), a clear read back or patched with no flush and MRT attachment 0 patched
# while bound (0389), an MRT scissored clear (0394), an MRT render continued after a flush by a
# discarding draw (0395), glCopyTexImage2D into GL_RGB (0398), and MRT framebuffers with a hole in
# their draw buffers ({0, NONE, 2} crashed before 0389; it and {NONE, 1} draw every attachment since
# 0396).
te=$(timeout 60 $OUT/texexact_probe 2>&1)
if echo "$te" | grep -q '^DIAG-END' && ! echo "$te" | grep -q '^DIAG BAD'; then
   ok "texel-exact sampling and odd widths"
else
   bad "texel-exact sampling: $(echo "$te" | grep '^DIAG BAD' | head -5 | tr '\n' ' ')"
fi

# GL_RGB (R8G8B8X8) textures and targets: X1-X3 sampling and partial upload, X4 an RGB render target
# read back and sampled (X1 and X4's sampling fail without 0388, X4's readback without 0389).
rx=$(timeout 60 $OUT/rgbx_probe 2>&1)
if echo "$rx" | grep -q '^RESULT: PASS' && ! echo "$rx" | grep -q '^SKIP'; then
   ok "RGB textures and targets (X1-X4)"
else
   bad "RGB textures and targets: $(echo "$rx" | grep '^FAIL' | head -5 | tr '\n' ' ')"
fi

# A fragment shader that can discard writes the pixels it keeps (every such shader wrote nothing
# before 0395).
dc=$(timeout 60 $OUT/discard_probe 2>&1)
if echo "$dc" | grep -q '^RESULT: PASS'; then
   ok "discard"
else
   bad "discard: $(echo "$dc" | grep '^DIAG BAD' | head -5 | tr '\n' ' ')"
fi
# No firmware context resets during the gate (CONTEXT_RESET_NOTIFICATION, which the kernel logs
# undecoded; a GPU page fault shows up only as one). rt8test raised two per run until 0393 (R8/RG8
# targets written at 4 bytes per pixel, past their end). A notice can arrive with a later job, so one
# more short probe runs first.
timeout 60 $OUT/fragcoord_diag >/dev/null 2>&1; sleep 3
# (plain journalctl shows no kernel lines to a user outside adm/systemd-journal, so it would report 0)
if log=$(sudo -n journalctl -k --since "$t0" 2>/dev/null); then
   nr=$(printf '%s\n' "$log" | grep -c 'FWCCB command 2abc0069')
   [ "$nr" = 0 ] && ok "no firmware context resets" || bad "firmware context resets: $nr"
else
   printf 'INFO firmware context resets during the gate: unknown (kernel log not readable)\n'
fi

if [ $fail = 0 ]; then echo "ALL GROUPS PASS"; else echo "SOME GROUPS FAILED"; fi
exit $fail
