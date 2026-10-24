#!/usr/bin/env bash
# Decode smoke for the media gate: one tiny committed stream per advertised codec through the
# real nv_video backend on the GPU, compared with pinned per-frame hashes (FNV-1a of the
# tightly packed display frame). The hashes were produced by test/real_codec_test.cpp runs
# that also compared every frame with an FFmpeg software decode; this script needs no ffmpeg.
#
#   make smoke NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>" [CODECS="h264 hevc vp9 av1"]
#
# CODECS is the advertised codec list (the gate maps media_codecs_c2.xml to these names).
# A codec the GPU does not support exits 77 from the test and is reported SKIP, not PASS.
# Needs /dev/dma_heap/system (root, as the gate runs).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-$HERE/../out/host/real_codec_test}"
STREAMS="$HERE/streams"
CODECS="${CODECS:-h264 hevc vp9 av1}"

[ -x "$BIN" ] || { echo "decode-smoke: missing $BIN (make out/host/real_codec_test NV_VIDEO_LIB=...)" >&2; exit 2; }

declare -A CASE=(
  [h264]="h264_320x180.h264 nv12"
  [hevc]="hevc_320x180.hevc nv12"
  [vp9]="vp9_320x180.ivf nv12"
  [av1]="av1_320x180.ivf nv12"
  [hevc10]="hevc10_320x180.hevc p010"
  [vp9p2]="vp9p2_320x180.ivf p010"
)
# codecs that have an extra 10-bit case (run only when the 8-bit codec is advertised)
TENBIT=("hevc:hevc10" "vp9:vp9p2")

crashes=0
ran=0
for codec in $CODECS; do
    set -- ${CASE[$codec]:-}
    [ $# -eq 2 ] || { echo "decode-smoke: unknown codec '$codec'" >&2; exit 2; }
    stream=$1 mode=$2
    out=$(REAL_MODE=$mode REAL_EXPECT="$STREAMS/${codec}_320x180.md5" \
          "$BIN" "${codec%10}" "$STREAMS/$stream" - 320 180 4 2>&1)
    rc=$?
    echo "$out" | sed 's/^/    /'
    case $rc in
        0) echo "decode-smoke: $codec ($mode) PASS"; ran=$((ran + 1)) ;;
        77) echo "decode-smoke: $codec ($mode) SKIP (not supported on this GPU)" ;;
        *) echo "decode-smoke: $codec ($mode) FAIL (exit $rc)"; crashes=$((crashes + 1)) ;;
    esac
done
for pair in "${TENBIT[@]}"; do
    base=${pair%%:*} ten=${pair##*:}
    case " $CODECS " in *" $base "*) ;; *) continue ;; esac
    set -- ${CASE[$ten]}
    stream=$1 mode=$2
    out=$(REAL_MODE=$mode REAL_EXPECT="$STREAMS/${ten}_320x180.md5" \
          "$BIN" "$base" "$STREAMS/$stream" - 320 180 4 2>&1)
    rc=$?
    echo "$out" | sed 's/^/    /'
    case $rc in
        0) echo "decode-smoke: $ten ($mode) PASS"; ran=$((ran + 1)) ;;
        77) echo "decode-smoke: $ten ($mode) SKIP (not supported on this GPU)" ;;
        *) echo "decode-smoke: $ten ($mode) FAIL (exit $rc)"; crashes=$((crashes + 1)) ;;
    esac
done

if [ $crashes -ne 0 ]; then
    echo "decode-smoke: FAIL ($crashes codec cases failed)" >&2
    exit 1
fi
if [ $ran -eq 0 ]; then
    echo "decode-smoke: no advertised codec decoded (SKIP everywhere)" >&2
    exit 1
fi
echo "decode-smoke: PASS ($ran codec cases)"
