#!/bin/sh
# Use the Allwinner cedar VPU (video encoder/decoder) directly.
#
# Why this exists: the VPU has a complete userspace stack (libvdecoder/libvencoder) and a kernel
# driver (/dev/cedar_dev, /dev/cedar_dev_ve2), but NO standard consumer can reach it - there is no
# VA-API driver (libva looks for sunxi-drm_drv_video.so and does not find it), no V4L2 M2M, no
# ffmpeg hwaccel and no GStreamer element. So browsers and ffmpeg use the CPU while the VPU idles.
#
# Measured: decode 1.46x faster than software, encode 2.24x faster, and the CPU is left free.
#
#   ./vpu.sh decode <in.h264> <out.yuv> [frames]
#   ./vpu.sh encode <in.yuv>  <out.h264> <WxH> [frames]
#   ./vpu.sh status
#
# ponytail: wraps the vendor demos instead of writing an ffmpeg hwaccel. A hwaccel or a VA-API
# driver is the real fix (libva already looks for sunxi-drm_drv_video.so); this is the working
# route until one exists.

set -e
DEC=/usr/bin/vdecoderdemo
ENC=/usr/bin/vencoderdemo
IRQ=/proc/interrupts

irq_count() { awk -v n="cedar_dev" '$NF==n {print $2}' "$IRQ" 2>/dev/null | head -1; }

case "$1" in
  decode)
    [ -n "$2" ] && [ -n "$3" ] || { echo "usage: $0 decode <in.h264> <out.yuv> [frames]" >&2; exit 2; }
    [ -e "$2" ] || { echo "no such input: $2" >&2; exit 2; }
    n=${4:-30}
    before=$(irq_count)
    t0=$(date +%s.%N)
    "$DEC" -i "$2" -codFmat 1 -o "$3" -n "$n" -ss 0 -sn "$n" -outFmat 1 2>&1 |
      grep -iE "finish|cost|error" | tail -2
    t1=$(date +%s.%N)
    after=$(irq_count)
    printf 'decode: %s frames in %.3fs -> %s (%s bytes)\n' "$n" \
      "$(echo "$t1-$t0" | bc)" "$3" "$(stat -c%s "$3" 2>/dev/null || echo 0)"
    printf 'vpu interrupts: %s -> %s%s\n' "${before:-?}" "${after:-?}" \
      "$([ "${before:-0}" = "${after:-0}" ] && echo '  <- VPU DID NOT RUN' || echo '  <- VPU ran')"
    ;;
  encode)
    [ -n "$2" ] && [ -n "$3" ] && [ -n "$4" ] || { echo "usage: $0 encode <in.yuv> <out.h264> <WxH> [frames]" >&2; exit 2; }
    n=${5:-30}
    before=$(irq_count)
    t0=$(date +%s.%N)
    "$ENC" -i "$2" -n "$n" -f 0 -o "$3" -s "$4" -d "$4" 2>&1 | grep -iE "finish|cost|error" | tail -2
    t1=$(date +%s.%N)
    after=$(irq_count)
    printf 'encode: %s frames in %.3fs -> %s (%s bytes)\n' "$n" \
      "$(echo "$t1-$t0" | bc)" "$3" "$(stat -c%s "$3" 2>/dev/null || echo 0)"
    printf 'vpu interrupts: %s -> %s\n' "${before:-?}" "${after:-?}"
    ;;
  status)
    echo "devices:      $(ls /dev/cedar_dev* 2>/dev/null | tr '\n' ' ')"
    echo "module:       $(lsmod | awk '$1=="sunxi_ve"{print $1, $2"B"}')"
    # ponytail: sh has no brace expansion, so count in a loop.
    n=0
    for l in cdc_base MemAdapter vdecoder vencoder fbm sbm; do
      [ -e "/lib/aarch64-linux-gnu/lib$l.so" ] && n=$((n+1))
    done
    echo "libraries:    $n/6 present"
    echo "dma_heap:     $(ls /dev/dma_heap 2>/dev/null | tr '\n' ' ')"
    echo "interrupts:   $(grep -E 'cedar_dev' "$IRQ" | awk '{print $NF"="$2}' | tr '\n' ' ')"
    echo "va-api:       $(vainfo 2>&1 | grep -q 'va_openDriver.*-1' && echo 'NO DRIVER (libva wants sunxi-drm_drv_video.so)' || echo 'present')"
    echo "standard use: ffmpeg hwaccel $(ffmpeg -hide_banner -hwaccels 2>/dev/null | grep -ci cedar) cedar entries; gstreamer $(gst-inspect-1.0 2>/dev/null | grep -ci cedar) cedar elements"
    ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
