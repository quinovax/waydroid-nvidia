#!/usr/bin/env bash
# packaging/install-from-release.sh — multi-distro installer for waydroid-nvidia
# Supports: Ubuntu/Debian, Fedora/RHEL, Arch Linux
#
# Two installation modes:
#   Release mode (default): downloads pre-built tarballs from a GitHub release.
#   Source mode (--source): builds everything from source at a given git tag.
#
# Usage:
#   curl -fsSL .../install-from-release.sh | sudo bash                       # latest release
#   curl -fsSL .../install-from-release.sh | sudo bash -s -- --tag v0.1.2    # specific release
#   sudo ./install-from-release.sh --source [--tag v0.1.0]                   # build from source
set -euo pipefail

REPO_URL="https://github.com/CinQwQeggs01/waydroid-nvidia"
WAYDROID_UPSTREAM="https://github.com/waydroid/waydroid.git"
PREFIX="/usr/lib/waydroid-nvidia"
TAG=""
SOURCE=0

die()  { echo "FATAL: $*" >&2; exit 1; }
info() { echo -e "\033[1;34m==>\033[0m $*"; }
ok()   { echo -e "\033[1;32m  OK\033[0m $*"; }

[ "$(id -u)" = 0 ] || die "must run as root"

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)    TAG="${2:?--tag needs a value}"; shift 2 ;;
        --source) SOURCE=1; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

# ---- detect distro ----
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu|debian|linuxmint|pop)    echo "debian" ;;
            fedora|rhel|centos|rocky|alma)  echo "fedora" ;;
            arch|manjaro|endeavouros)       echo "arch"   ;;
            *) die "unsupported distro: $ID (supported: Ubuntu/Debian, Fedora, Arch)" ;;
        esac
    elif command -v apt-get >/dev/null 2>&1; then echo "debian"
    elif command -v dnf >/dev/null 2>&1; then echo "fedora"
    elif command -v pacman >/dev/null 2>&1; then echo "arch"
    else die "cannot detect distro"
    fi
}

DISTRO=$(detect_distro)
info "detected distro family: $DISTRO"

# ---- pipewire-pulseaudio conflict guard ----
PA_PKG="pulseaudio"
case "$DISTRO" in
    debian) dpkg -s pipewire-pulseaudio &>/dev/null && PA_PKG="" ;;
    fedora) rpm -q  pipewire-pulseaudio &>/dev/null && PA_PKG="" ;;
    arch)   pacman -Qi pipewire-pulseaudio &>/dev/null && PA_PKG="" ;;
esac
[ -z "$PA_PKG" ] && info "pipewire-pulseaudio detected — skipping traditional pulseaudio package"

# ---- runtime deps (both modes) ----
install_deps_debian() {
    info "installing Debian/Ubuntu dependencies"
    apt-get update -q
    # shellcheck disable=SC2086
    apt-get install -yq lxc python3 python3-gi python3-dbus nftables dnsmasq \
        gir1.2-gtk-3.0 $PA_PKG binutils \
        libepoxy0 libdrm2 libgbm1 libx11-6 libexpat1 libvulkan1 \
        curl git make gcc patch
}

install_deps_fedora() {
    info "installing Fedora dependencies"
    # shellcheck disable=SC2086
    dnf install -yq lxc python3 python3-gobject python3-dbus nftables dnsmasq \
        gtk3 $PA_PKG binutils \
        libepoxy libdrm mesa-libgbm libX11 expat vulkan-loader \
        curl git make gcc patch
}

install_deps_arch() {
    info "installing Arch dependencies"
    # shellcheck disable=SC2086
    pacman -Syq --noconfirm --needed lxc python python-gobject python-dbus \
        nftables dnsmasq gtk3 $PA_PKG binutils \
        libepoxy libdrm mesa libx11 expat vulkan-icd-loader \
        curl git base-devel patch
}

case "$DISTRO" in
    debian) install_deps_debian ;;
    fedora) install_deps_fedora ;;
    arch)   install_deps_arch   ;;
esac
ok "dependencies installed"

# ---- build deps (source mode only) ----
install_build_deps_debian() {
    info "installing Debian/Ubuntu build dependencies"
    apt-get update -q
    apt-get install -yq meson ninja-build gcc g++ cmake pkg-config \
        libdrm-dev libgbm-dev libepoxy-dev vulkan-headers \
        libwayland-dev wayland-protocols \
        libx11-dev libxrandr-dev libxfixes-dev x11proto-dev \
        python3-yaml python3-mako python3-packaging \
        glslang-tools libffi-dev libssl-dev \
        libsystemd-dev libcap-dev lzip xz-utils unzip
}

install_build_deps_fedora() {
    info "installing Fedora build dependencies"
    dnf install -yq meson ninja-build gcc gcc-c++ cmake pkg-config \
        libdrm-devel libgbm-devel libepoxy-devel vulkan-headers \
        wayland-devel wayland-protocols-devel \
        libX11-devel libXrandr-devel libXfixes-devel xorg-x11-proto-devel \
        python3-pyyaml python3-mako python3-packaging \
        glslang libffi-devel openssl-devel \
        systemd-devel libcap-devel lzip xz unzip
}

install_build_deps_arch() {
    info "installing Arch build dependencies"
    pacman -Syq --noconfirm --needed meson ninja gcc cmake pkgconf \
        libdrm libgbm libepoxy vulkan-headers \
        wayland wayland-protocols \
        libx11 libxrandr libxfixes \
        python-mako python-packaging python-yaml \
        glslang libffi openssl \
        systemd-libs libcap lzip xz unzip
}

# ---- source build ----
build_from_source() {
    local work="$1" repo_src="$2"
    info "building from source (this may take a while)"

    case "$DISTRO" in
        debian) install_build_deps_debian ;;
        fedora) install_build_deps_fedora ;;
        arch)   install_build_deps_arch   ;;
    esac
    ok "build dependencies installed"

    # ---- Android NDK (needed for guest mesa/venus x86 + x86_64) ----
    local pins="$repo_src/packaging/ci/pins.env"
    local ndk_ver ndk_sha
    ndk_ver=$(grep '^NDK_VERSION=' "$pins" | cut -d= -f2)
    ndk_sha=$(grep '^NDK_ZIP_SHA256=' "$pins" | cut -d= -f2)
    local NDK_DIR="${ANDROID_NDK_HOME:-/opt/android-ndk}"
    if [ ! -x "$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android34-clang" ]; then
        info "downloading Android NDK $ndk_ver"
        local ndk_zip="$work/android-ndk-${ndk_ver}-linux.zip"
        curl -fL --retry 3 -o "$ndk_zip" "https://dl.google.com/android/repository/android-ndk-${ndk_ver}-linux.zip"
        echo "$ndk_sha  $ndk_zip" | sha256sum -c - || die "NDK checksum mismatch"
        mkdir -p "$(dirname "$NDK_DIR")"
        unzip -q "$ndk_zip" -d "$(dirname "$NDK_DIR")"
        mv "$(dirname "$NDK_DIR")/android-ndk-$ndk_ver" "$NDK_DIR"
        rm "$ndk_zip"
        ok "NDK installed to $NDK_DIR"
    else
        ok "NDK already at $NDK_DIR"
    fi
    export NDK="$NDK_DIR"

    # ---- clone upstream source trees ----
    local WNV="$work/src"
    mkdir -p "$WNV"

    clone_tree() {
        local name="$1" url="$2" sha="$3"
        local dir="$WNV/$name"
        info "cloning $name @ ${sha:0:12}"
        git clone -q "$url" "$dir"
        git -C "$dir" checkout -q "$sha"
        ok "$name ready"
    }

    local MESA_SHA VIRGL_SHA
    MESA_SHA=$(grep '^MESA_SHA=' "$pins" | cut -d= -f2)
    VIRGL_SHA=$(grep '^VIRGL_SHA=' "$pins" | cut -d= -f2)

    clone_tree mesa \
        "$(grep '^MESA_UPSTREAM=' "$pins" | cut -d= -f2)" \
        "$MESA_SHA"

    clone_tree virglrenderer \
        "$(grep '^VIRGL_UPSTREAM=' "$pins" | cut -d= -f2)" \
        "$VIRGL_SHA"

    # ---- build using the repo's canonical build recipes ----
    export REPO="$repo_src"

    info "building virglrenderer (host renderer)"
    REPO="$repo_src" "$repo_src/build/virglrenderer/build.sh" "$WNV/virglrenderer" "$WNV/virglrenderer/build"
    ok "virglrenderer built"

    info "building mesa (guest Venus, x86_64)"
    ANDROID_ABI=x86_64 "$repo_src/build/mesa/build.sh" "$WNV/mesa" "$WNV/mesa/build-android-x86_64"
    ok "mesa (x86_64) built"

    info "building mesa (guest Venus, x86)"
    ANDROID_ABI=x86 "$repo_src/build/mesa/build.sh" "$WNV/mesa" "$WNV/mesa/build-android-x86"
    ok "mesa (x86) built"

    # ---- install built artifacts ----
    info "installing built host binaries to $PREFIX"
    mkdir -p "$PREFIX"
    local virgl_built="$WNV/virglrenderer/build"
    install -Dm755 "$virgl_built/vtest/virgl_test_server"       "$PREFIX/virgl_test_server"
    install -Dm755 "$virgl_built/server/virgl_render_server"    "$PREFIX/virgl_render_server"
    local so
    for so in "$virgl_built/src/libvirglrenderer.so."*; do
        [ -f "$so" ] && install -Dm644 "$so" "$PREFIX/$(basename "$so")"
    done

    info "installing built guest binaries to $PREFIX/guest"
    mkdir -p "$PREFIX/guest/vendor/lib64/hw" "$PREFIX/guest/vendor/lib/hw"
    install -Dm644 "$WNV/mesa/build-android-x86_64/src/virtio/vulkan/libvulkan_virtio.so" \
        "$PREFIX/guest/vendor/lib64/hw/vulkan.virtio.so"
    install -Dm644 "$WNV/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so" \
        "$PREFIX/guest/vendor/lib/hw/vulkan.virtio.so"

    ok "source build and install complete"
    info "NOTE: ANGLE, hwcomposer, and surfaceflinger are not built by --source."
    info "If the release for this tag includes a prebuilts tarball, re-run without --source;"
    info "otherwise copy them manually into $PREFIX/guest/"
}

# ---- determine tag ----
if [ -z "$TAG" ]; then
    if [ "$SOURCE" -eq 1 ]; then
        # source mode with no tag: use latest tag
        TAG=$(git ls-remote --tags --sort=-v:refname "$REPO_URL" | head -1 | sed 's/.*refs\/tags\///')
    else
        # release mode with no tag: use latest release
        TAG=$(curl -fsSL "https://api.github.com/repos/CinQwQeggs01/waydroid-nvidia/releases/latest" | \
              grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//' | sed 's/".*//')
    fi
    [ -n "$TAG" ] || die "could not determine tag (no releases? use --tag or --source)"
fi
info "using tag: $TAG"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- clone repo at tag (needed for both modes) ----
info "cloning repo at $TAG"
REPO_SRC="$WORK/waydroid-nvidia"
git clone -q --depth 1 --branch "$TAG" "$REPO_URL" "$REPO_SRC"

if [ "$SOURCE" -eq 1 ]; then
    # ---- SOURCE MODE: build everything from source ----
    build_from_source "$WORK" "$REPO_SRC"
else
    # ---- RELEASE MODE: download pre-built tarballs ----
    BASE_URL="$REPO_URL/releases/download/$TAG"

    info "downloading release tarballs"
    for f in waydroid-nvidia-host-x86_64-${TAG}.tar.gz \
             waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz \
             SHA256SUMS; do
        curl -fSL "$BASE_URL/$f" -o "$WORK/$f" || \
            die "failed to download $f — release may not exist for $TAG (try --source)"
    done
    # prebuilts (ANGLE, hwcomposer, surfaceflinger) — optional
    if curl -fSL "$BASE_URL/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" \
            -o "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" 2>/dev/null; then
        info "prebuilts (ANGLE/hwcomposer/surfaceflinger) downloaded"
    else
        info "prebuilts not available in this release — GL will use software fallback"
    fi

    info "verifying checksums"
    (cd "$WORK" && sha256sum -c --ignore-missing SHA256SUMS) || die "SHA256 verification failed"
    ok "checksums verified"

    info "installing host binaries to $PREFIX"
    mkdir -p "$PREFIX"
    tar -C "$PREFIX" -xf "$WORK/waydroid-nvidia-host-x86_64-${TAG}.tar.gz"

    info "installing guest binaries to $PREFIX/guest"
    mkdir -p "$PREFIX/guest"
    tar -C "$PREFIX/guest" -xf "$WORK/waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz"
    if [ -f "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" ]; then
        tar -C "$PREFIX/guest" -xf "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz"
    fi
    ok "binaries installed"
fi

# ---- install patched waydroid (both modes) ----
info "installing patched waydroid"
WAYDROID_SHA=$(sed -n 's/^base-commit: *//p' "$REPO_SRC/patches/waydroid/BASE" | sed 's/ .*//')
WAYDROID_SRC="$WORK/waydroid"
git init -q "$WAYDROID_SRC"
git -C "$WAYDROID_SRC" fetch -q --depth 1 "$WAYDROID_UPSTREAM" "$WAYDROID_SHA"
git -C "$WAYDROID_SRC" checkout -q FETCH_HEAD
git -C "$WAYDROID_SRC" apply "$REPO_SRC/patches/waydroid/0001-nvidia-integration.patch"

NFTABLES=1
command -v nft >/dev/null 2>&1 || NFTABLES=0
make -C "$WAYDROID_SRC" install USE_NFTABLES=$NFTABLES
ok "patched waydroid installed"

# ---- install systemd units, udev rules, tmpfiles, setup script ----
info "installing host integration files"
P="$REPO_SRC/packaging/aur/waydroid-nvidia-bin"

install -Dm644 "$P/wd-venus.service"        /usr/lib/systemd/user/wd-venus.service
install -Dm644 "$P/waydroid-venus.tmpfiles" /usr/lib/tmpfiles.d/waydroid-venus.conf
install -Dm644 "$P/waydroid-nvidia.rules"   /usr/lib/udev/rules.d/70-waydroid-nvidia.rules
install -Dm755 "$P/waydroid-nvidia-setup"   /usr/bin/waydroid-nvidia-setup

# ---- install SELinux policy (Fedora/RHEL only) ----
if command -v semodule >/dev/null 2>&1; then
    info "installing SELinux policy module"
    if command -v checkmodule >/dev/null 2>&1; then
        checkmodule -M -m -o "$WORK/waydroid-nvidia.mod" \
            "$REPO_SRC/packaging/selinux/waydroid-nvidia.te" 2>/dev/null && \
        semodule_package -o "$WORK/waydroid-nvidia.pp" \
            -m "$WORK/waydroid-nvidia.mod" 2>/dev/null && \
        semodule -i "$WORK/waydroid-nvidia.pp" 2>/dev/null && \
        ok "SELinux policy installed" || info "SELinux policy install failed (non-fatal)"
    fi
fi

# ---- activate tmpfiles and udev ----
systemd-tmpfiles --create /usr/lib/tmpfiles.d/waydroid-venus.conf 2>/dev/null || true
udevadm control --reload 2>/dev/null || true
udevadm trigger /dev/udmabuf 2>/dev/null || true

ok "installation complete"
cat <<'EOF'

Next steps:
  waydroid init                     # download an Android image
  sudo waydroid-nvidia-setup        # configure the NVIDIA stack (--refresh <hz>)
  sudo systemctl enable --now waydroid-container.service
  systemctl --user enable --now wd-venus.service
  # re-login once (udev rule for /dev/udmabuf), then:
  waydroid session start

Verify GPU acceleration:
  sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
  # should show: ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))

EOF
