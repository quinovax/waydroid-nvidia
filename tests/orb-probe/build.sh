#!/bin/sh
# Build orb-probe: three compute shaders -> SPIR-V -> embedded header -> one C file.
#
# Two binaries are produced from the SAME source (see README.md):
#   build/orb-probe-host     host glibc, dlopens libvulkan at runtime
#   build/orb-probe-android  x86_64 Android/bionic, for the waydroid guest
#
# Nothing here touches the GPU: compile only. Do not run it during a benchmark
# window (compiling is CPU work, and bench windows are sacred -- see README).
#
# env overrides: GLSLANG, ANDROID_NDK_ROOT, CC_HOST, CC_ANDROID, BUILD,
#                HOST_ONLY=1, GUEST_ONLY=1, SPV_ONLY=1
set -eu

cd "$(dirname "$0")"

GLSLANG="${GLSLANG:-glslangValidator}"
NDK="${ANDROID_NDK_ROOT:-/opt/android-ndk}"
CC_HOST="${CC_HOST:-gcc}"
CC_ANDROID="${CC_ANDROID:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang}"
BUILD="${BUILD:-build}"

HOST_ONLY="${HOST_ONLY:-0}"
GUEST_ONLY="${GUEST_ONLY:-0}"
SPV_ONLY="${SPV_ONLY:-0}"

command -v "$GLSLANG" >/dev/null 2>&1 || { echo "missing $GLSLANG" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "missing python3" >&2; exit 2; }

mkdir -p "$BUILD"

echo "== SPIR-V"
for s in dispatch_a prepare_args dispatch_b; do
    # vulkan1.1 target keeps the modules loadable on both the 580 ICD and NVK
    "$GLSLANG" -V --target-env vulkan1.1 -o "$BUILD/$s.spv" "shaders/$s.comp"
    echo "   $BUILD/$s.spv"
done

echo "== embedded header"
python3 gen_spv_header.py "$BUILD" "$BUILD/shaders_spv.h"

if [ "$SPV_ONLY" = "1" ]; then
    echo "SPV_ONLY=1, stopping after SPIR-V"
    exit 0
fi

CFLAGS="${CFLAGS:--O2 -std=c99 -Wall -Wextra}"

if [ "$GUEST_ONLY" != "1" ]; then
    echo "== host build ($CC_HOST)"
    # -ldl is the only link dependency: libvulkan is dlopen()ed, so the same
    # source links unchanged on bionic where there is no libvulkan.so stub.
    $CC_HOST $CFLAGS -I"$BUILD" -o "$BUILD/orb-probe-host" orb_probe.c -ldl
    ls -l "$BUILD/orb-probe-host"
fi

if [ "$HOST_ONLY" != "1" ]; then
    if [ ! -x "$CC_ANDROID" ]; then
        echo "WARNING: android clang not found at $CC_ANDROID -- skipping guest build" >&2
        echo "         set ANDROID_NDK_ROOT or CC_ANDROID (NDK r30 lives at /opt/android-ndk)" >&2
    else
        echo "== guest build ($CC_ANDROID)"
        $CC_ANDROID $CFLAGS -I"$BUILD" -o "$BUILD/orb-probe-android" orb_probe.c -ldl
        ls -l "$BUILD/orb-probe-android"
    fi
fi

echo "ok"
