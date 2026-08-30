#!/usr/bin/env bash
# Deploy this checkout with a single pkexec (no sudo, no chained elevations).
#
# Default: keep host binaries already in /usr/lib/waydroid-nvidia, refresh
# patches / units / guest prebuilts from this tree, copy a local virgl/mesa
# rebuild if present, then run waydroid-nvidia-setup.
#
#   ./pkexec-install.sh
#   ./pkexec-install.sh --source     # also rebuild mesa + virglrenderer

set -euo pipefail

usage() {
    echo "usage: $0 [--source]" >&2
    exit 2
}

# Privileged half. Invoked only via pkexec; arguments are explicit because
# pkexec wipes the environment.
as_root() {
    local repo="" home="" source=0
    local prefix=/usr/lib/waydroid-nvidia
    local virgl_build="" mesa_vk_x64=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --repo)        repo="${2:?}"; shift 2 ;;
            --home)        home="${2:?}"; shift 2 ;;
            --source)      source=1; shift ;;
            --virgl-build) virgl_build="${2:?}"; shift 2 ;;
            --mesa-vk)     mesa_vk_x64="${2:?}"; shift 2 ;;
            *) echo "pkexec-install --as-root: unknown argument: $1" >&2; exit 2 ;;
        esac
    done
    [ -n "$repo" ] && [ -d "$repo/patches/waydroid" ] || {
        echo "pkexec-install: --repo is not a waydroid-nvidia checkout" >&2
        exit 1
    }
    [ "$(id -u)" = 0 ] || { echo "pkexec-install: --as-root must run as root" >&2; exit 1; }

    local script="$repo/packaging/install-from-release.sh"
    if [ "$source" -eq 1 ]; then
        "$script" --local "$repo" --source
    else
        "$script" --local "$repo" --skip-build
    fi

    if [ -z "$virgl_build" ] && [ -n "$home" ]; then
        virgl_build="$home/waydroid-nv/virglrenderer/build"
    fi
    if [ -z "$mesa_vk_x64" ] && [ -n "$home" ]; then
        mesa_vk_x64="$home/waydroid-nv/mesa/build-android-x86_64/src/virtio/vulkan/libvulkan_virtio.so"
    fi

    if [ -n "$virgl_build" ] && [ -x "$virgl_build/vtest/virgl_test_server" ]; then
        echo "== installing host renderer from $virgl_build"
        install -Dm755 "$virgl_build/vtest/virgl_test_server" "$prefix/virgl_test_server"
        if [ -x "$virgl_build/server/virgl_render_server" ]; then
            install -Dm755 "$virgl_build/server/virgl_render_server" "$prefix/virgl_render_server"
        fi
        if [ -e "$virgl_build/src/libvirglrenderer.so.1" ]; then
            install -Dm644 "$virgl_build/src/libvirglrenderer.so.1" "$prefix/libvirglrenderer.so.1"
        fi
        local so
        so=$(ls "$virgl_build/src"/libvirglrenderer.so.1.* 2>/dev/null | head -1 || true)
        if [ -n "$so" ]; then
            install -Dm644 "$so" "$prefix/$(basename "$so")"
        fi
    fi

    if [ -n "$mesa_vk_x64" ] && [ -f "$mesa_vk_x64" ]; then
        echo "== installing guest Venus (x86_64) from $mesa_vk_x64"
        install -Dm644 "$mesa_vk_x64" "$prefix/guest/vendor/lib64/hw/vulkan.virtio.so"
    fi

    if [ -x /usr/bin/waydroid-nvidia-setup ] && [ -f /var/lib/waydroid/waydroid.cfg ]; then
        echo "== running waydroid-nvidia-setup"
        /usr/bin/waydroid-nvidia-setup
    else
        echo "== skipping waydroid-nvidia-setup (waydroid not initialized)"
    fi

    echo "Deployment complete."
}

if [ "${1:-}" = "--as-root" ]; then
    shift
    as_root "$@"
    exit 0
fi

SOURCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --source) SOURCE=1; shift ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

REPO="$(cd "$(dirname "$0")" && pwd)"
HOME_DIR="${HOME:-}"
VIRGL_BUILD="${VIRGL_BUILD:-$HOME_DIR/waydroid-nv/virglrenderer/build}"
MESA_VK_X64="${MESA_VK_X64:-$HOME_DIR/waydroid-nv/mesa/build-android-x86_64/src/virtio/vulkan/libvulkan_virtio.so}"

ROOT_ARGS=(--as-root --repo "$REPO")
[ -n "$HOME_DIR" ] && ROOT_ARGS+=(--home "$HOME_DIR")
[ -d "$VIRGL_BUILD" ] && ROOT_ARGS+=(--virgl-build "$VIRGL_BUILD")
[ -f "$MESA_VK_X64" ] && ROOT_ARGS+=(--mesa-vk "$MESA_VK_X64")
[ "$SOURCE" -eq 1 ] && ROOT_ARGS+=(--source)

echo "Running install with one pkexec (local checkout)..."
# pkexec /bin/bash rather than the repo script: the wrapper is user-writable.
exec pkexec --keep-cwd /bin/bash "$REPO/pkexec-install.sh" "${ROOT_ARGS[@]}"
