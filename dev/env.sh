#!/usr/bin/env bash
# env.sh — single source of truth for the dev loop's tree locations, build
# targets, deploy destinations and patch-regen anchors. Sourced by every dev/
# script. Everything is overridable from the environment, e.g.
#   WAYDROID_SRC=/somewhere/else dev/restart
#
# The three trees this repo orchestrates (see docs/dev-workflow.md):
#   REPO         this repo (patches/ + src/ + build glue)
#   WAYDROID_SRC the runtime waydroid python checkout (waydroid.py, live lxc.py)
#   WNV/*        native component build trees (mesa / virgl / hwcomposer / angle)

set -euo pipefail

# --- this repo (dir containing dev/); overridable for testing ---
: "${REPO:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# --- external trees ---
# NOTE: this account was renamed and the trees moved; these defaults used to
# point at $HOME/repos/{waydroid-nv,waydroid}, which no longer exist.  WNV is
# ~/waydroid-nv.  There is no per-user waydroid.py checkout under $HOME: the
# runtime is the Fedora package's /usr/lib/waydroid (root-owned, so
# `git -C ... diff` cannot run there and sync-patches' waydroid patch has to
# come from the writable git checkout in /tmp/wdsrc).
: "${WNV:=$HOME/waydroid-nv}"
: "${WAYDROID_SRC:=/usr/lib/waydroid}"
# Writable git checkout of the same waydroid tree, used only as the patch anchor.
: "${WAYDROID_PATCH_TREE:=/tmp/wdsrc}"

: "${MESA_TREE:=$WNV/mesa}"
: "${MESA_BUILD_X86_64:=${MESA_BUILD:-$MESA_TREE/build-android-x86_64}}"
: "${MESA_BUILD_X86:=$MESA_TREE/build-android-x86}"
# Backward-compatible name used by older local helpers.
: "${MESA_BUILD:=$MESA_BUILD_X86_64}"
: "${VIRGL_TREE:=$WNV/virglrenderer}"
: "${VIRGL_BUILD:=$VIRGL_TREE/build}"
# HWC_TREE must be the tree carrying patches/hwcomposer/0001.  hwcomposer-src is
# a pristine upstream clone, so pointing sync-patches at it captures nothing
# (and previously produced an empty 0001, silently dropping every HAL fix).
# It is the git ROOT (the patch paths are hwcomposer/... relative to it);
# HWC_SRCDIR is the subdir build/hwcomposer/build.sh compiles.
: "${HWC_TREE:=$WNV/android_hardware_waydroid}"
: "${HWC_SRCDIR:=$HWC_TREE/hwcomposer}"
: "${HWC_BUILD:=$WNV/hwc-build}"
: "${ANGLE_TREE:=$WNV/angle-src}"
: "${ANGLE_OUT_X86_64:=$ANGLE_TREE/out/AndroidX64}"
: "${ANGLE_OUT_X86:=$ANGLE_TREE/out/AndroidX86}"

# --- toolchain ---
: "${NDK:=/opt/android-ndk}"
: "${NDK_BIN:=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin}"
: "${STRIP:=$NDK_BIN/llvm-strip}"

# --- runtime ---
: "${LXC:=-P /var/lib/waydroid/lxc -n waydroid}"
: "${VENUS_UNIT:=wd-venus.service}"
: "${CONTAINER_UNIT:=wd-container.service}"
: "${SESSION_UNIT:=wd-session.service}"
: "${DEPLOY:=/usr/local/sbin/wd-deploy}"

# --- patch-regen anchors (verified ancestors; see dev/sync-patches) ---
: "${MESA_BASE:=a8ce4d8}"
: "${VIRGL_BASE:=dc35e4d}"
: "${HWC_BASE:=7750307}"
: "${WAYDROID_BASE:=a33a5c0}"

# --- helpers ---
say()  { printf '\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31mxx %s\033[0m\n' "$*" >&2; exit 1; }

# guest shell (root inside the container), env cleaned so exec sh never fails.
# stdio goes through pipes, NOT the caller's fds: lxc-attach chowns its std
# fds to the container root (attach.c fix_stdio_permissions), which turns any
# redirect-target file into an unreadable root-owned 600 file.
guest() {
    sudo -n lxc-attach $LXC --clear-env -v PATH=/system/bin -- /system/bin/sh -c "$*" \
        < /dev/null 2> >(cat >&2) | cat
}

# The waydroid-dev sudoers allowlist grants NOPASSWD for exactly the commands
# the loop uses (lxc-attach/lxc-info/systemctl wd-container/wd-deploy) — probe
# one of those, NOT `sudo -n true`, which is not allowlisted and would demand
# cached credentials the loop doesn't actually need.
have_sudo() { sudo -n lxc-info --version >/dev/null 2>&1; }
need_sudo() { have_sudo || die "sudo -n unavailable — install the waydroid-dev sudoers allowlist or run 'sudo -v' first"; }
