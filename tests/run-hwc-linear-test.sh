#!/usr/bin/env bash
# Build and run the LINEAR-dmabuf admission test for the hwcomposer HAL.
# Needs no GPU, no guest and no root: the predicates under test
# (compositor_supports / compositor_uses_nvidia_egl) are copied from
# patches/hwcomposer/0001-wip-nvidia-fixes.patch so a regression in that logic
# fails here instead of aborting the HAL on a user's machine.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cxx="${CXX:-g++}"
"$cxx" -O0 -Wall -o "$out/t" "$here/hwc-linear-buffer.c"
"$out/t"
