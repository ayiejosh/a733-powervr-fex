#!/bin/bash
# regress.sh - run the whole Vulkan test suite against one ICD and print one table.
#
# The compiler in Mesa's pvr driver is shared by every shader the ICD compiles, so
# a change there is only safe if all of these still pass:
#   vktest     compute execution + readback verification
#   vkrender   offscreen render pass + resolve + copy + pixel check. The pixel
#              check only runs with both phases (MODE defaults to "both"), so the
#              format / sample-count / size cases leave MODE alone; the partial
#              modes are run separately as "executed cleanly" checks.
#   vkaudit    the advertised capability set (limits, extensions, features)
#   memtypes   the memory types and readback behaviour
#   bda        buffer device addresses (descriptor and push-constant delivery)
#   pctest     minimal vkCmdPushConstants probe, independent of buffer addresses
#   glheadless zink GL over the same ICD, when a GL stack is present
#
# Usage:
#   ./regress.sh                       # uses VK_ICD_FILENAMES/VK_DRIVER_FILES as-is
#   VK_ICD_FILENAMES=<icd.json> ./regress.sh
#
# Exits non-zero if any case failed, so it can gate a commit.
set -u
cd "$(dirname "$0")"

for t in vktest vkrender vkaudit memtypes bda pctest; do
  [ -x "./$t" ] || { echo "regress.sh: ./$t missing - run ./build.sh"; exit 2; }
done

pass=0
fail=0
known=0
declare -a failed_cases=()
declare -a known_cases=()

# verdict: the test verified its own output and must say PASS.
# clean:   the tool reports figures rather than a verdict; it must simply run
#          without failing.
# known:   a case that fails for a reason already established to predate this
#          work (it also fails on the pre-change ICD, or on the vendor driver).
#          Reported, listed separately, and not counted against the suite - but
#          never hidden: the failing lines are printed either way.
run_case() {
  local kind="$1" label="$2"; shift 2
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done
  shift

  local out rc verdict
  out=$(env "${envs[@]}" timeout 300 "$@" 2>&1)
  rc=$?

  if [ $rc -ne 0 ]; then
    verdict="FAIL(rc=$rc)"
  elif printf '%s' "$out" | grep -qE "VERDICT: FAIL|RESULT: FAIL|^FAIL"; then
    verdict="FAIL"
  elif [ "$kind" = "verdict" ]; then
    if printf '%s' "$out" | grep -qE "VERDICT: PASS|^PASS \("; then
      verdict="PASS"
    else
      verdict="?(no verdict)"
    fi
  else
    verdict="ok"
  fi

  if [ "$verdict" = "PASS" ] || [ "$verdict" = "ok" ]; then
    pass=$((pass + 1))
  elif [ "$kind" = "known" ]; then
    known=$((known + 1))
    known_cases+=("$label")
    printf '%s\n' "$out" | grep -E "FAIL|first (bad|mismatch)|VERDICT" | head -4 | sed 's/^/      /'
    verdict="KNOWN-FAIL"
  else
    fail=$((fail + 1))
    failed_cases+=("$label")
    printf '%s\n' "$out" | grep -E "FAIL|VERDICT|RESULT|first (bad|mismatch)|error|skipped" | head -6 | sed 's/^/      /'
  fi
  printf '  %-44s %s\n' "$label" "$verdict"
}

echo "icd: ${VK_ICD_FILENAMES:-<default>}"
echo
echo "== compute =="
run_case verdict "vktest compute+readback" -- ./vktest 3 262144

echo "== render (pixel verification) =="
run_case verdict "vkrender 512" -- ./vkrender 512 4
run_case verdict "vkrender 512 samples=1" SAMPLES=1 -- ./vkrender 512 4
run_case verdict "vkrender 512 samples=2" SAMPLES=2 -- ./vkrender 512 4
run_case verdict "vkrender 512 samples=4" SAMPLES=4 -- ./vkrender 512 4
run_case verdict "vkrender 1024" -- ./vkrender 1024 4
run_case verdict "vkrender 2048" -- ./vkrender 2048 2
# The verifier used to compare four channels for every format, so these could
# never pass; it now checks each format at its own width, and the driver renders
# all of them correctly.
run_case verdict "vkrender 512 r8" FORMAT=r8 -- ./vkrender 512 4
run_case verdict "vkrender 512 r16" FORMAT=r16 -- ./vkrender 512 4
run_case verdict "vkrender 512 rg16" FORMAT=rg16 -- ./vkrender 512 4
# Partial modes exercise the individual passes; they do not self-verify.
run_case clean "vkrender 512 copy-only" MODE=copy -- ./vkrender 512 4
run_case clean "vkrender 512 empty pass" MODE=empty -- ./vkrender 512 4

echo "== memory =="
run_case clean "memtypes" -- ./memtypes

echo "== capabilities =="
run_case clean "vkaudit" -- ./vkaudit

echo "== buffer device address =="
# Advertised by default now: both delivery mechanisms, all three access modes.
run_case verdict "bda (descriptor + push constant)" -- ./bda

echo "== push constants =="
run_case verdict "pctest (vkCmdPushConstants)" -- ./pctest

# GL/zink drives the same ICD through a second compiler path, so it is a real
# regression target rather than an optional extra. It needs nullDescriptor from
# VK_KHR_robustness2, which the vendor ICD does not expose, so skip it there
# rather than reporting a failure that says nothing about this driver.
GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
if printf '%s' "${VK_ICD_FILENAMES:-}" | grep -q "img_icd"; then
  echo "== GL (zink over this ICD) =="
  echo "  skipped: the vendor ICD has no nullDescriptor, so zink cannot start on it"
elif [ -x ./glheadless ] && [ -d /home/radxa/mesa/gldri ]; then
  echo "== GL (zink over this ICD) =="
  run_case verdict "glheadless 512x20" \
    LD_LIBRARY_PATH="$GL_PREFIX" \
    LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
    GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
    MESA_LOADER_DRIVER_OVERRIDE=zink \
    EGL_PLATFORM=gbm \
    DRM_RENDER_NODE=/dev/dri/renderD128 \
    MESA_GLES_VERSION_OVERRIDE=3.2 \
    -- ./glheadless 512 20
fi

# These used to fail intermittently and were blamed on the driver. They were a
# missing pipeline barrier in this tool: a layout transition does not order the
# render pass's writes against the copy's reads, and the vendor driver fails 5/5
# without the barrier and passes 5/5 with it. With the barrier in place the
# whole range is expected to be correct, including the sizes the extent fix
# enabled.
echo "== large render targets =="
run_case verdict "vkrender 4096 (max before the extent fix)" -- ./vkrender 4096 1
run_case verdict "vkrender 6144" -- ./vkrender 6144 1
run_case verdict "vkrender 8192" -- ./vkrender 8192 1

echo
echo "regress: $pass passed, $fail failed, $known known-open"
if [ "$known" -ne 0 ]; then
  printf 'known-open (pre-existing, not a regression): %s\n' "${known_cases[*]}"
fi
if [ "$fail" -ne 0 ]; then
  printf 'failed: %s\n' "${failed_cases[*]}"
  exit 1
fi
exit 0
