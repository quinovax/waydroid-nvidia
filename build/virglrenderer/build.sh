#!/usr/bin/env bash
# build/virglrenderer/build.sh SRCDIR [BUILDDIR]
# Build the host Venus renderer (virgl_test_server + render server). The ONE
# virgl build recipe: drops the net-new vtest allocator source into the tree,
# then meson + ninja. Used by dev/build and packaging/reproduce.sh.
#
# The net-new src (src/virglrenderer-vtest/vtest_gpu_alloc.{c,h}) is referenced
# by the vtest/meson.build change in patch 0006, so it must sit in vtest/.
set -euo pipefail
SRCDIR="${1:?usage: build.sh SRCDIR [BUILDDIR]}"
BUILDDIR="${2:-$SRCDIR/build}"
# repo root = three levels up from this script (build/virglrenderer/build.sh)
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"

echo "virgl/build.sh: install net-new vtest allocator source"
mkdir -p "$SRCDIR/vtest/"

# src/ is canonical: installing it is what makes the tree build the reviewed
# copy.  But if the tree already holds a DIFFERENT copy, that copy is unsynced
# work and this install destroys it silently -- measured: an entire feature
# (the WDRDIAG_ALLOC_TRACE allocator hook) disappeared that way and the next
# build died on an undefined symbol.  Warn loudly and name the way out.
install_netnew() {
    if [ -f "$2" ] && ! cmp -s "$1" "$2"; then
        {
            echo "virgl/build.sh: WARNING: $2"
            echo "  differs from $1 — src/ is canonical and will overwrite it."
            echo "  If the tree copy is the newer one, run dev/sync-patches FIRST"
            echo "  (it regenerates patches/ and src/ from the tree)."
        } >&2
    fi
    install -m 0644 "$1" "$2"
}

install_netnew "$REPO/src/virglrenderer-vtest/vtest_gpu_alloc.c"  "$SRCDIR/vtest/vtest_gpu_alloc.c"
install_netnew "$REPO/src/virglrenderer-vtest/vtest_gpu_alloc.h"  "$SRCDIR/vtest/vtest_gpu_alloc.h"
install_netnew "$REPO/src/vtest_alloc_formats.h"                  "$SRCDIR/vtest/vtest_alloc_formats.h"

if [ ! -f "$BUILDDIR/build.ninja" ]; then
    echo "virgl/build.sh: fresh meson setup -> $BUILDDIR"
    meson setup "$BUILDDIR" "$SRCDIR" -Dvenus=true -Drender-server-worker=auto
fi

ninja -C "$BUILDDIR"
echo "  -> $BUILDDIR/vtest/virgl_test_server (+ server/virgl_render_server)"
