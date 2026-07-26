#!/usr/bin/env bash
# packaging/fedora/build.sh — Generic Fedora build script for waydroid-nvidia
# Builds: mesa (guest Venus), virglrenderer (host renderer), gralloc, hwcomposer
# Requires: Fedora 40+ with official repos only (no COPR/external)
# SELinux: sets correct contexts on installed binaries
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
PINFILE="$REPO/packaging/ci/pins.env"
source "$PINFILE"

: "${WNV:=$HOME/waydroid-nv}"
: "${NDK:=$HOME/android-ndk}"
: "${INSTALL_PREFIX:=/usr/lib/waydroid-nvidia}"
: "${GUEST_PREFIX:=/var/lib/waydroid/overlay/system}"

die()  { echo "FATAL: $*" >&2; exit 1; }
info() { echo -e "\033[1;34m==>\033[0m $*"; }
ok()   { echo -e "\033[1;32m  OK\033[0m $*"; }

# ---- dependencies ----
DEPS=(
  meson ninja-build gcc gcc-c++ git cmake
  libdrm-devel libgbm-devel libepoxy-devel vulkan-headers
  wayland-devel wayland-protocols-devel
  libX11-devel libXrandr-devel libXfixes-devel xorg-x11-proto-devel
  python3-pyyaml python3-mako python3-packaging
  glslang
  libffi-devel openssl-devel
  systemd-devel libcap-devel
  lzip xz
)

install_deps() {
  info "Installing build dependencies"
  sudo dnf install -y "${DEPS[@]}"
  ok "Dependencies installed"
}

# ---- NDK ----
setup_ndk() {
  if [[ -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android34-clang" ]]; then
    ok "NDK already at $NDK"; return
  fi
  info "Downloading Android NDK $NDK_VERSION"
  local zip="android-ndk-${NDK_VERSION}-linux.zip"
  local url="https://dl.google.com/android/repository/$zip"
  curl -fL --retry 3 -o "/tmp/$zip" "$url"
  echo "$NDK_ZIP_SHA256  /tmp/$zip" | sha256sum -c - || die "NDK checksum mismatch"
  mkdir -p "$(dirname "$NDK")"
  unzip -q "/tmp/$zip" -d "$(dirname "$NDK")"
  mv "$(dirname "$NDK")/android-ndk-$NDK_VERSION" "$NDK"
  rm "/tmp/$zip"
  ok "NDK installed to $NDK"
}

# ---- source trees ----
clone_tree() {
  local name="$1" url="$2" sha="$3"
  local dir="$WNV/$name"
  if [[ -d "$dir/.git" ]]; then
    info "Updating $name"
    git -C "$dir" fetch --all -q
  else
    info "Cloning $name"
    git clone "$url" "$dir"
  fi
  git -C "$dir" checkout -q "$sha"
  ok "$name @ ${sha:0:12}"
}

setup_sources() {
  mkdir -p "$WNV"
  clone_tree mesa "$MESA_UPSTREAM" "$MESA_SHA"
  clone_tree virglrenderer "$VIRGL_UPSTREAM" "$VIRGL_SHA"
  clone_tree minigbm "$MINIGBM_UPSTREAM" "$MINIGBM_SHA"
  clone_tree hwcomposer-src "$WAYDROID_UPSTREAM" "$WAYDROID_SHA"
}

# ---- patch and build each component ----

build_mesa() {
  info "Building mesa (guest Venus driver)"
  local src="$WNV/mesa"
  git -C "$src" checkout -q "$MESA_SHA"
  git -C "$src" am -q "$REPO/patches/mesa/0001-"*.patch "$REPO/patches/mesa/0002-"*.patch 2>/dev/null || \
    git -C "$src" am --skip  # already applied
  git -C "$src" apply "$REPO/patches/mesa/0003-wip-"*.patch 2>/dev/null || true
  REPO="$REPO" "$REPO/build/mesa/build.sh" "$src" "$src/build-android-x86_64"
  ok "mesa built"
}

build_virgl() {
  info "Building virglrenderer (host renderer)"
  local src="$WNV/virglrenderer"
  git -C "$src" checkout -q "$VIRGL_SHA"
  git -C "$src" am -q "$REPO/patches/virglrenderer/0001-"*.patch \
                     "$REPO/patches/virglrenderer/0002-"*.patch 2>/dev/null || \
    git -C "$src" am --skip
  git -C "$src" am -q "$REPO/patches/virglrenderer/0003-"*.patch 2>/dev/null || \
    git -C "$src" am --skip
  git -C "$src" apply "$REPO/patches/virglrenderer/0004-wip-"*.patch 2>/dev/null || true
  REPO="$REPO" "$REPO/build/virglrenderer/build.sh" "$src" "$src/build"
  ok "virglrenderer built"
}

build_gralloc() {
  info "Building gralloc backend"
  REPO="$REPO" "$REPO/build/gralloc/build.sh" "$WNV/hwc-build/out"
  ok "gralloc built"
}

build_hwcomposer() {
  info "Building hwcomposer"
  "$REPO/build/hwcomposer/build.sh" "$WNV/hwcomposer-src/hwcomposer" "$WNV/hwc-build/out"
  ok "hwcomposer built"
}

# ---- install ----
install_all() {
  info "Installing to $INSTALL_PREFIX"
  sudo mkdir -p "$INSTALL_PREFIX" "$INSTALL_PREFIX/guest"
  sudo cp -f "$WNV/virglrenderer/build/vtest/virgl_test_server" "$INSTALL_PREFIX/"
  sudo cp -f "$WNV/virglrenderer/build/server/virgl_render_server" "$INSTALL_PREFIX/"
  sudo cp -f "$WNV/virglrenderer/build/src/libvirglrenderer.so."* "$INSTALL_PREFIX/"
  sudo cp -f "$WNV/hwc-build/out/libEGL_angle.so" "$INSTALL_PREFIX/guest/" 2>/dev/null || true
  sudo cp -f "$WNV/hwc-build/out/libGLESv2_angle.so" "$INSTALL_PREFIX/guest/" 2>/dev/null || true
  sudo cp -f "$WNV/hwc-build/out/libGLESv1_CM_angle.so" "$INSTALL_PREFIX/guest/" 2>/dev/null || true
  sudo cp -f "$WNV/mesa/build-android-x86_64/src/virtio/vulkan/libvulkan_virtio.so" "$INSTALL_PREFIX/guest/" 2>/dev/null || true
  sudo cp -f "$WNV/hwc-build/out/hwcomposer.waydroid.so" "$INSTALL_PREFIX/guest/" 2>/dev/null || true
  sudo chmod 755 "$INSTALL_PREFIX/virgl_test_server" "$INSTALL_PREFIX/virgl_render_server"
  sudo chmod 644 "$INSTALL_PREFIX/"*.so* "$INSTALL_PREFIX/guest/"*.so 2>/dev/null || true

  # SELinux contexts for host binaries (virgl_test_server runs as user service)
  if command -v restorecon &>/dev/null; then
    info "Restoring SELinux contexts"
    sudo restorecon -Rv "$INSTALL_PREFIX/" 2>/dev/null || true
  fi
  ok "Installed to $INSTALL_PREFIX"
}

# ---- main ----
case "${1:-all}" in
  deps)     install_deps ;;
  ndk)      setup_ndk ;;
  sources)  setup_sources ;;
  mesa)     build_mesa ;;
  virgl)    build_virgl ;;
  gralloc)  build_gralloc ;;
  hwc)      build_hwcomposer ;;
  install)  install_all ;;
  all)
    install_deps
    setup_ndk
    setup_sources
    build_virgl
    build_mesa
    build_gralloc
    build_hwcomposer
    install_all
    echo
    ok "All components built and installed."
    echo "Restart venus: systemctl --user restart wd-venus.service"
    echo "Restart container: sudo systemctl restart waydroid-container.service"
    echo "Restart session: waydroid session stop && waydroid session start"
    ;;
  *) die "Usage: $0 {deps|ndk|sources|mesa|virgl|gralloc|hwc|install|all}" ;;
esac
