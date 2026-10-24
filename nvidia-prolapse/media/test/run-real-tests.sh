#!/usr/bin/env bash
# Every codec / output layout of nvc2::Decoder on the real nv_video backend and GPU.
# Generates its own 640x360, 60-frame streams (testsrc2) and FFmpeg software references,
# so the comparison is against an independently decoded picture, not a pinned dump.
#
#   make realtest-all NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>"
#   REAL_DATA=<dir> test/run-real-tests.sh        # keep/regenerate data elsewhere
#
# Cases: H.264, HEVC, VP9, AV1 in NV12; H.264, HEVC, VP9 in the Codec2 component's YV12
# (ByteBuffer/CPU_READ) target; HEVC Main10 and VP9 profile 2 in P010. Needs ffmpeg with
# libx264, libx265, libvpx-vp9 and libsvtav1, and /dev/dma_heap/system (root).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-$HERE/../out/host/real_codec_test}"
DATA="${REAL_DATA:-$HERE/../out/real-tests}"
W=640 H=360 FRAMES=60
[ -x "$BIN" ] || { echo "run-real-tests: missing $BIN (make out/host/real_codec_test NV_VIDEO_LIB=...)" >&2; exit 2; }
for tool in ffmpeg ffprobe; do command -v $tool >/dev/null || { echo "run-real-tests: $tool missing" >&2; exit 2; }; done
mkdir -p "$DATA"
SRC="$DATA/src.y4m"
[ -s "$SRC" ] || ffmpeg -v error -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=24" -frames:v "$FRAMES" -pix_fmt yuv420p "$SRC"

fails=0 runs=0
# name codec encoder-args container ref-decoder pix_fmt mode
case_run() {
    name=$1 codec=$2 enc=$3 ext=$4 refdec=$5 pix=$6 mode=$7
    stream="$DATA/$name.$ext" ref="$DATA/$name.$pix"
    [ -s "$stream" ] || eval "ffmpeg -v error -y -i \"$SRC\" $enc -f $ext \"$stream\"" || { echo "encode $name failed"; fails=$((fails+1)); return; }
    [ -s "$ref" ] || ffmpeg -v error -y -c:v "$refdec" -i "$stream" -fps_mode passthrough -f rawvideo -pix_fmt "$pix" "$ref" \
        || { echo "reference $name failed"; fails=$((fails+1)); return; }
    out=$(REAL_MODE=$mode "$BIN" "$codec" "$stream" "$ref" "$W" "$H" 4 2>&1)
    rc=$?
    echo "$out" | tail -2 | sed 's/^/    /'
    case $rc in
        0) echo "real-tests: $name ($mode) PASS"; runs=$((runs+1)) ;;
        77) echo "real-tests: $name ($mode) SKIP (not supported on this GPU)" ;;
        *) echo "real-tests: $name ($mode) FAIL (exit $rc)"; fails=$((fails+1)) ;;
    esac
}
X264="-c:v libx264 -preset veryfast -profile:v high -bf 3 -g 60 -crf 20"
X265="-c:v libx265 -preset fast -x265-params bframes=4:log-level=error -g 60 -crf 22"
VP9="-c:v libvpx-vp9 -cpu-used 4 -row-mt 1 -g 60 -crf 32 -b:v 0"
AV1="-c:v libsvtav1 -preset 10 -g 60 -crf 38"

case_run h264 h264 "$X264" h264 h264 nv12 nv12
case_run hevc hevc "$X265" hevc hevc nv12 nv12
case_run vp9 vp9 "$VP9" ivf libvpx-vp9 nv12 nv12
case_run av1 av1 "$AV1" ivf libdav1d nv12 nv12
case_run h264_yv12 h264 "$X264" h264 h264 nv12 yv12
case_run hevc_yv12 hevc "$X265" hevc hevc nv12 yv12
case_run vp9_yv12 vp9 "$VP9" ivf libvpx-vp9 nv12 yv12
case_run hevc_main10 hevc "$X265 -pix_fmt yuv420p10le" hevc hevc p010le p010
case_run vp9_p2 vp9 "$VP9 -profile:v 2 -pix_fmt yuv420p10le" ivf libvpx-vp9 p010le p010

if [ $fails -ne 0 ]; then echo "real-tests: FAIL ($fails cases)"; exit 1; fi
if [ $runs -eq 0 ]; then echo "real-tests: no case ran"; exit 1; fi
echo "real-tests: PASS ($runs cases)"
