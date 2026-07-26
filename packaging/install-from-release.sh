#!/usr/bin/env bash
# packaging/install-from-release.sh — multi-distro installer for waydroid-nvidia
# Supports: Ubuntu/Debian, Fedora/RHEL, Arch Linux
# Usage: curl -fsSL https://raw.githubusercontent.com/Shiro836/waydroid-nvidia/main/packaging/install-from-release.sh | sudo bash
# Or:    sudo ./packaging/install-from-release.sh [--tag vX.Y.Z]
set -euo pipefail

REPO_URL="https://github.com/Shiro836/waydroid-nvidia"
WAYDROID_UPSTREAM="https://github.com/waydroid/waydroid.git"
WAYDROID_SHA="a33a5c0b31d89d6ce687381104b30aff4dd2d330"
PREFIX="/usr/lib/waydroid-nvidia"
TAG=""

die()  { echo "FATAL: $*" >&2; exit 1; }
info() { echo -e "\033[1;34m==>\033[0m $*"; }
ok()   { echo -e "\033[1;32m  OK\033[0m $*"; }

[ "$(id -u)" = 0 ] || die "must run as root"

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="${2:?--tag needs a value}"; shift 2 ;;
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

# ---- install deps ----
install_deps_debian() {
    info "installing Debian/Ubuntu dependencies"
    apt-get update -q
    apt-get install -yq lxc python3 python3-gi python3-dbus nftables dnsmasq \
        gir1.2-gtk-3.0 pulseaudio binutils \
        libepoxy0 libdrm2 libgbm1 libx11-6 libexpat1 libvulkan1 \
        curl git make gcc
}

install_deps_fedora() {
    info "installing Fedora dependencies"
    dnf install -yq lxc python3 python3-gobject python3-dbus nftables dnsmasq \
        gtk3 pulseaudio binutils \
        libepoxy libdrm mesa-libgbm libX11 expat vulkan-loader \
        curl git make gcc
}

install_deps_arch() {
    info "installing Arch dependencies"
    pacman -Syq --noconfirm --needed lxc python python-gobject python-dbus \
        nftables dnsmasq gtk3 pulseaudio binutils \
        libepoxy libdrm mesa libx11 expat vulkan-icd-loader \
        curl git base-devel
}

case "$DISTRO" in
    debian) install_deps_debian ;;
    fedora) install_deps_fedora ;;
    arch)   install_deps_arch   ;;
esac
ok "dependencies installed"

# ---- fetch latest release tag if not specified ----
if [ -z "$TAG" ]; then
    TAG=$(curl -fsSL "https://api.github.com/repos/Shiro836/waydroid-nvidia/releases/latest" | \
          grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//' | sed 's/".*//')
    [ -n "$TAG" ] || die "could not determine latest release tag"
fi
info "using release: $TAG"

BASE_URL="$REPO_URL/releases/download/$TAG"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- download and verify ----
info "downloading release tarballs"
for f in waydroid-nvidia-host-x86_64-${TAG}.tar.gz \
         waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz \
         waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz \
         SHA256SUMS; do
    curl -fSL "$BASE_URL/$f" -o "$WORK/$f"
done

info "verifying checksums"
(cd "$WORK" && sha256sum -c --ignore-missing SHA256SUMS) || die "SHA256 verification failed"
ok "checksums verified"

# ---- install binaries ----
info "installing host binaries to $PREFIX"
mkdir -p "$PREFIX"
tar -C "$PREFIX" -xf "$WORK/waydroid-nvidia-host-x86_64-${TAG}.tar.gz"

info "installing guest binaries to $PREFIX/guest"
mkdir -p "$PREFIX/guest"
tar -C "$PREFIX/guest" -xf "$WORK/waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz"
tar -C "$PREFIX/guest" -xf "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz"
ok "binaries installed"

# ---- install patched waydroid ----
info "installing patched waydroid"
WAYDROID_SRC="$WORK/waydroid"
git init -q "$WAYDROID_SRC"
git -C "$WAYDROID_SRC" fetch -q --depth 1 "$WAYDROID_UPSTREAM" "$WAYDROID_SHA"
git -C "$WAYDROID_SRC" checkout -q FETCH_HEAD

# Download the nvidia integration patch from the release's source tarball
PATCH_URL="$REPO_URL/raw/main/patches/waydroid/0001-nvidia-integration.patch"
curl -fsSL "$PATCH_URL" -o "$WORK/nvidia-integration.patch"
git -C "$WAYDROID_SRC" apply "$WORK/nvidia-integration.patch"

NFTABLES=1
command -v nft >/dev/null 2>&1 || NFTABLES=0
make -C "$WAYDROID_SRC" install USE_NFTABLES=$NFTABLES
ok "patched waydroid installed"

# ---- install systemd units, udev rules, tmpfiles, setup script ----
info "installing host integration files"
REPO_RAW="$REPO_URL/raw/main"

curl -fsSL "$REPO_RAW/packaging/aur/waydroid-nvidia-bin/wd-venus.service" \
    -o "$WORK/wd-venus.service"
install -Dm644 "$WORK/wd-venus.service" /usr/lib/systemd/user/wd-venus.service

curl -fsSL "$REPO_RAW/packaging/aur/waydroid-nvidia-bin/waydroid-venus.tmpfiles" \
    -o "$WORK/waydroid-venus.conf"
install -Dm644 "$WORK/waydroid-venus.conf" /usr/lib/tmpfiles.d/waydroid-venus.conf

curl -fsSL "$REPO_RAW/packaging/aur/waydroid-nvidia-bin/waydroid-nvidia.rules" \
    -o "$WORK/70-waydroid-nvidia.rules"
install -Dm644 "$WORK/70-waydroid-nvidia.rules" /usr/lib/udev/rules.d/70-waydroid-nvidia.rules

curl -fsSL "$REPO_RAW/packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup" \
    -o "$WORK/waydroid-nvidia-setup"
install -Dm755 "$WORK/waydroid-nvidia-setup" /usr/bin/waydroid-nvidia-setup

# ---- install SELinux policy (Fedora/RHEL only) ----
if command -v semodule >/dev/null 2>&1; then
    info "installing SELinux policy module"
    curl -fsSL "$REPO_RAW/packaging/selinux/waydroid-nvidia.te" -o "$WORK/waydroid-nvidia.te"
    if command -v checkmodule >/dev/null 2>&1; then
        checkmodule -M -m -o "$WORK/waydroid-nvidia.mod" "$WORK/waydroid-nvidia.te" 2>/dev/null && \
        semodule_package -o "$WORK/waydroid-nvidia.pp" -m "$WORK/waydroid-nvidia.mod" 2>/dev/null && \
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
