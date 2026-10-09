#!/bin/sh
# Use the Allwinner cedar VPU (video encoder/decoder) directly.
#
# Why this exists: the VPU has a complete userspace stack (libvdecoder/libvencoder) and a kernel
# driver (/dev/cedar_dev, /dev/cedar_dev_ve2), but NO standard consumer can reach it - there is no
# VA-API driver (libva looks for sunxi-drm_drv_video.so and does not find it), no V4L2 M2M, no
# ffmpeg hwaccel and no GStreamer element. So browsers and ffmpeg use the CPU while the VPU idles.
#
# Measured, and CORRECTED twice: the VPU does NOT decode faster than the CPU. On 300 frames of 720p
# both take ~3.7 ms/frame (VPU 1.25 s, ffmpeg 1.12 s). The earlier 1.46x came from a 36-frame test
# where vdecoderdemo's fixed startup dominated. The real finding is that the device works and
# NOTHING standard can reach it - no VA-API driver, no V4L2 M2M, no ffmpeg hwaccel, no GStreamer.
# BUT the decode-any pipeline is 1.39x SLOWER end-to-end than plain ffmpeg. The overhead is NOT
# disk I/O (writing to tmpfs made it worse) and NOT the hardware - it is the vendor demo's own
# per-frame processing, which reports 'cost 0 s' internally while wall time is 1.6 s. This is a
demonstration
# and a fallback, NOT a speedup. A real win needs a VA-API driver or an ffmpeg hwaccel.
#
#   ./vpu.sh decode <in.h264> <out.yuv> [frames]
#   ./vpu.sh decode-any <in.mp4|mkv|anything> <out.yuv> [frames]
#       ffmpeg demuxes to a raw H.264 elementary stream, then the VPU decodes it. This is the
#       reachable unlock for real files: ffmpeg cannot hand a decoder to us, but it CAN demux.
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
  decode-any)
    # ponytail: two-stage pipe with a temp elementary stream. ffmpeg demuxes (CPU, cheap), the
    # VPU decodes (the expensive part). A real hwaccel would avoid the temp file; this works now.
    [ -n "$2" ] && [ -n "$3" ] || { echo "usage: $0 decode-any <in.any> <out.yuv> [frames]" >&2; exit 2; }
    [ -e "$2" ] || { echo "no such input: $2" >&2; exit 2; }
    n=${4:-30}
    tmp=$(mktemp /tmp/vpuany.XXXXXX.h264)
    trap 'rm -f "$tmp"' EXIT
    echo "demux: ffmpeg -> $tmp"
    ffmpeg -hide_banner -loglevel error -i "$2" -c:v copy -bsf:v h264_mp4toannexb -f h264 "$tmp" -y 2>&1 | head -3
    [ -s "$tmp" ] || { echo "demux produced nothing - is the video H.264?" >&2; exit 1; }
    printf 'elementary stream: %s bytes\n' "$(stat -c%s "$tmp")"
    "$0" decode "$tmp" "$3" "$n"
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
