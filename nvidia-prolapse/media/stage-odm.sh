#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Stage the /odm tree of the NVDEC Codec2 HAL for an Android 13 image whose vendor.img has
# an (almost) empty /odm (Waydroid LineageOS 20: /odm/* are symlinks to /vendor/odm/*, and
# vendor.img's /odm holds only etc/{build.prop,group,passwd}). The result replaces
# /vendor/odm with one bind mount:
#
#   OUT/etc/build.prop, group, passwd          copied unchanged from vendor.img /odm/etc
#   OUT/etc/init/*.rc                          service definition
#   OUT/etc/vintf/manifest/*.xml               IComponentStore/default (ODM manifest fragment)
#   OUT/etc/seccomp_policy/*.policy            minijail policy the service applies
#   OUT/etc/media_codecs_c2.xml                generated NOW from this GPU (nvc2-xmlgen)
#   OUT/bin/hw/android.hardware.media.c2@1.2-service-nvprolapse
#
# usage: stage-odm.sh OUT VENDOR_IMG SERVICE_BINARY NVC2_XMLGEN
# Every step fails loudly; nothing is skipped.
set -euo pipefail
# Tool output parsed here and in every child (readelf, debugfs, lxc-info, systemctl, ...) is the untranslated text
export LC_ALL=C
unset LANGUAGE
[ $# -eq 4 ] || { echo "usage: $0 OUT VENDOR_IMG SERVICE_BINARY NVC2_XMLGEN" >&2; exit 2; }
OUT=$1 VENDOR_IMG=$2 SERVICE=$3 XMLGEN=$4
HERE=$(cd "$(dirname "$0")" && pwd)

[ -s "$SERVICE" ] || { echo "stage-odm: service binary $SERVICE missing" >&2; exit 1; }
[ -x "$XMLGEN" ] || { echo "stage-odm: $XMLGEN is not executable (make -C $HERE xmlgen NV_VIDEO_LIB=...)" >&2; exit 1; }
listing=$(debugfs -R "ls -p /odm/etc" "$VENDOR_IMG" 2>/dev/null | awk -F/ 'NF>5 && $6!="." && $6!=".." {print $6}' | sort | tr '\n' ' ')
[ "$listing" = "build.prop group passwd " ] || {
    echo "stage-odm: vendor.img /odm/etc holds '$listing', expected exactly 'build.prop group passwd';" \
         "the bind over /vendor/odm would hide files this script does not know" >&2; exit 1; }
debugfs -R "ls -p /odm" "$VENDOR_IMG" 2>/dev/null | awk -F/ 'NF>5 && $6!="." && $6!=".." {print $6}' | grep -qvx etc && {
    echo "stage-odm: vendor.img /odm has entries besides etc/" >&2; exit 1; } || true

rm -rf "$OUT"
mkdir -p "$OUT/etc" "$OUT/bin/hw"
for f in build.prop group passwd; do
    debugfs -R "dump -p /odm/etc/$f $OUT/etc/$f" "$VENDOR_IMG" >/dev/null 2>&1
    [ -e "$OUT/etc/$f" ] || { echo "stage-odm: cannot copy /odm/etc/$f from $VENDOR_IMG" >&2; exit 1; }
done
cp -r "$HERE/odm/etc/init" "$HERE/odm/etc/vintf" "$HERE/odm/etc/seccomp_policy" "$OUT/etc/"
"$XMLGEN" -o "$OUT/etc/media_codecs_c2.xml"
python3 -I "$HERE/test/validate_xml.py" "$OUT/etc/media_codecs_c2.xml"
install -m 0755 "$SERVICE" "$OUT/bin/hw/android.hardware.media.c2@1.2-service-nvprolapse"
chmod 0644 "$OUT"/etc/init/*.rc "$OUT"/etc/vintf/manifest/*.xml "$OUT"/etc/seccomp_policy/*.policy \
    "$OUT/etc/media_codecs_c2.xml"
echo "stage-odm: staged $OUT"
find "$OUT" -type f | sort
