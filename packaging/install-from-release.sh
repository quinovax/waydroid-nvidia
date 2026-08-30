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
#   curl -fsSL .../install-from-release.sh | sudo bash -s -- --tag v0.1.1    # specific release
#   sudo ./install-from-release.sh --source [--tag v0.1.0]                   # build from source
#   pkexec ./packaging/install-from-release.sh --local "$PWD" --skip-build   # deploy this checkout
set -euo pipefail

REPO_URL="https://github.com/CinQwQeggs01/waydroid-nvidia"
WAYDROID_UPSTREAM="https://github.com/waydroid/waydroid.git"
PREFIX="/usr/lib/waydroid-nvidia"
TAG=""
SOURCE=0
LOCAL=""
SKIP_BUILD=0
# ANGLE / hwcomposer / surfaceflinger are built on a self-hosted runner this
# fork does not have. When a tag ships without guest-prebuilts, reuse this
# repository's last release that includes them.
PREBUILTS_FALLBACK_URL="https://github.com/CinQwQeggs01/waydroid-nvidia/releases/download/v0.1.1"
PREBUILTS_FALLBACK_FILE="waydroid-nvidia-guest-prebuilts-v0.1.1.tar.gz"
PREBUILTS_FALLBACK_SHA256="a3f49671dd460134f38433c7a89b8916bc95329c68f413bd94c1c22a5ab48ca6"

die()  { echo "FATAL: $*" >&2; exit 1; }
info() { echo -e "\033[1;34m==>\033[0m $*"; }
ok()   { echo -e "\033[1;32m  OK\033[0m $*"; }

[ "$(id -u)" = 0 ] || die "must run as root"

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)        TAG="${2:?--tag needs a value}"; shift 2 ;;
        --source)     SOURCE=1; shift ;;
        --local)      LOCAL="${2:?--local needs a path}"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

if [ -n "$LOCAL" ]; then
    [ -d "$LOCAL/patches/waydroid" ] || die "--local $LOCAL is not a waydroid-nvidia checkout"
    LOCAL="$(cd "$LOCAL" && pwd)"
fi

# ---- detect package-manager family ----
# Attribute a system file to apt, dnf, or pacman.
detect_pkg_family() {
    local probe="" f hits="" family
    for f in /usr/lib/os-release /etc/os-release /usr/bin/bash /bin/sh; do
        if [ -e "$f" ]; then
            probe="$f"
            break
        fi
    done
    [ -n "$probe" ] || die "cannot find a system file to attribute to a package manager"

    # Refuse if more than one manager claims $probe.
    family=""
    if command -v dpkg-query >/dev/null 2>&1 && \
            dpkg-query -S "$probe" >/dev/null 2>&1; then
        hits="${hits:+$hits }debian"
        family=debian
    fi
    if command -v rpm >/dev/null 2>&1 && \
            rpm -q --whatprovides "$probe" >/dev/null 2>&1; then
        hits="${hits:+$hits }fedora"
        family=fedora
    fi
    if command -v pacman >/dev/null 2>&1 && \
            pacman -Qo "$probe" >/dev/null 2>&1; then
        hits="${hits:+$hits }arch"
        family=arch
    fi
    case "$hits" in
        debian|fedora|arch)
            echo "$family"
            return 0
            ;;
        "")
            ;;
        *)
            die "ambiguous package manager for $probe (got: $hits). Need exactly one of dpkg, rpm, pacman."
            ;;
    esac

    # Chroots / debootstrap / rpmstrap where the probe file is not packaged.
    if command -v apt-get >/dev/null 2>&1 && [ -f /var/lib/dpkg/status ]; then
        echo debian
        return 0
    fi
    if { command -v dnf || command -v yum; } >/dev/null 2>&1 && \
            { [ -d /usr/lib/sysimage/rpm ] || [ -d /var/lib/rpm ]; }; then
        echo fedora
        return 0
    fi
    if command -v pacman >/dev/null 2>&1 && [ -d /var/lib/pacman/local ]; then
        echo arch
        return 0
    fi
    die "cannot detect package manager (need apt+dpkg, dnf/yum+rpm, or pacman)"
}

DISTRO=$(detect_pkg_family)
info "package manager family: $DISTRO"

# ---- pipewire-pulseaudio conflict guard ----
PA_PKG="pulseaudio"
case "$DISTRO" in
    debian) dpkg -s pipewire-pulseaudio &>/dev/null && PA_PKG="" ;;
    fedora) rpm -q  pipewire-pulseaudio &>/dev/null && PA_PKG="" ;;
    arch)   pacman -Qi pipewire-pulse &>/dev/null && PA_PKG="" ;;
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
        curl git make gcc patch zstd
    # python3-gbinder only exists in the Waydroid PPA / newer Debian; waydroid
    # imports it at runtime, so try it but do not fail the whole install here.
    apt-get install -yq python3-gbinder 2>/dev/null \
        || info "python3-gbinder unavailable — add the Waydroid PPA and install it manually"
}

install_deps_fedora() {
    info "installing Fedora dependencies"
    # shellcheck disable=SC2086
    dnf install -yq lxc python3 python3-gobject python3-dbus python3-gbinder \
        nftables dnsmasq gtk3 $PA_PKG binutils \
        libepoxy libdrm mesa-libgbm libX11 expat vulkan-loader \
        curl git make gcc patch zstd
}

install_deps_arch() {
    info "installing Arch dependencies"
    # shellcheck disable=SC2086
    pacman -Syq --noconfirm --needed lxc python python-gobject python-dbus \
        python-gbinder nftables dnsmasq gtk3 $PA_PKG binutils \
        libepoxy libdrm mesa libx11 expat vulkan-icd-loader \
        curl git base-devel patch zstd
}

if [ "$SKIP_BUILD" -eq 1 ]; then
    command -v git >/dev/null 2>&1 || die "git is required"
    command -v make >/dev/null 2>&1 || die "make is required"
    command -v zstd >/dev/null 2>&1 || die "zstd is required to unpack the prebuilts fallback"
    command -v tar >/dev/null 2>&1 || die "tar is required"
    info "skip-build: not installing distro packages"
else
    case "$DISTRO" in
        debian) install_deps_debian ;;
        fedora) install_deps_fedora ;;
        arch)   install_deps_arch   ;;
    esac
    ok "dependencies installed"
fi

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
        libdrm mesa libepoxy vulkan-headers \
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
    info "ANGLE, hwcomposer, and surfaceflinger are not built by --source;"
    info "the installer will fetch the guest-prebuilts tarball next."
}

extract_archive() {
    local archive="$1" dest="$2"
    mkdir -p "$dest"
    case "$archive" in
        *.tar.zst|*.tzst) tar --zstd -C "$dest" -xf "$archive" ;;
        *.tar.gz|*.tgz)   tar -C "$dest" -xf "$archive" ;;
        *) die "unsupported archive: $archive" ;;
    esac
}

# ANGLE / hwcomposer / surfaceflinger. waydroid-nvidia-setup hard-requires them
# (issues #2 and #5); never continue with a half-empty guest tree.
install_prebuilts() {
    local dest="$1"
    mkdir -p "$dest"
    if [ -f "$dest/vendor/lib64/egl/libEGL_angle.so" ] &&
       [ -f "$dest/vendor/lib/egl/libEGL_angle.so" ] &&
       [ -f "$dest/vendor/lib64/hw/hwcomposer.waydroid.so" ] &&
       [ -f "$dest/system/bin/surfaceflinger" ]; then
        ok "guest prebuilts already present"
        return 0
    fi

    local tag_tb=""
    if [ -n "${TAG:-}" ]; then
        for cand in "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" \
                    "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.zst"; do
            [ -f "$cand" ] && tag_tb="$cand" && break
        done
    fi
    if [ -n "$tag_tb" ]; then
        info "installing prebuilts from $tag_tb"
        extract_archive "$tag_tb" "$dest"
    fi

    if [ ! -f "$dest/vendor/lib64/egl/libEGL_angle.so" ] ||
       [ ! -f "$dest/vendor/lib/egl/libEGL_angle.so" ] ||
       [ ! -f "$dest/vendor/lib64/hw/hwcomposer.waydroid.so" ] ||
       [ ! -f "$dest/system/bin/surfaceflinger" ]; then
        info "fetching prebuilts fallback ($PREBUILTS_FALLBACK_FILE)"
        local fb="$WORK/$PREBUILTS_FALLBACK_FILE"
        curl -fSL "$PREBUILTS_FALLBACK_URL/$PREBUILTS_FALLBACK_FILE" -o "$fb" || \
            die "failed to download prebuilts fallback from $PREBUILTS_FALLBACK_URL"
        echo "$PREBUILTS_FALLBACK_SHA256  $fb" | sha256sum -c - || \
            die "prebuilts fallback checksum mismatch"
        extract_archive "$fb" "$dest"
    fi

    [ -f "$dest/vendor/lib64/egl/libEGL_angle.so" ] || \
        die "prebuilts missing vendor/lib64/egl/libEGL_angle.so"
    [ -f "$dest/vendor/lib/egl/libEGL_angle.so" ] || \
        die "prebuilts missing vendor/lib/egl/libEGL_angle.so (32-bit ANGLE)"
    [ -f "$dest/vendor/lib64/hw/hwcomposer.waydroid.so" ] || \
        die "prebuilts missing vendor/lib64/hw/hwcomposer.waydroid.so"
    [ -f "$dest/system/bin/surfaceflinger" ] || \
        die "prebuilts missing system/bin/surfaceflinger"
    ok "guest prebuilts installed (ANGLE/hwcomposer/surfaceflinger)"
}

# ---- determine tag ----
if [ "$SKIP_BUILD" -eq 0 ] && [ -z "$TAG" ]; then
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
if [ -n "$TAG" ]; then
    info "using tag: $TAG"
elif [ "$SKIP_BUILD" -eq 1 ]; then
    info "skip-build: using binaries already in $PREFIX"
else
    die "could not determine tag (no releases? use --tag or --source)"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- clone repo for patches + integration files ----
# --local: this checkout. Release mode: fetch from main so patches/BASE are
# always current. Source mode: fetch at the tag so the build matches the release.
if [ -n "$LOCAL" ]; then
    REPO_SRC="$LOCAL"
    info "using local checkout $REPO_SRC"
else
    REPO_SRC="$WORK/waydroid-nvidia"
    if [ "$SOURCE" -eq 1 ]; then
        info "cloning repo at $TAG"
        git clone -q --depth 1 --branch "$TAG" "$REPO_URL" "$REPO_SRC"
    else
        info "cloning repo (main)"
        git clone -q --depth 1 "$REPO_URL" "$REPO_SRC"
    fi
fi

if [ "$SKIP_BUILD" -eq 1 ]; then
    [ -x "$PREFIX/virgl_test_server" ] || \
        die "--skip-build requires existing $PREFIX/virgl_test_server (install a release first)"
    info "keeping existing host/guest binaries in $PREFIX"
    mkdir -p "$PREFIX/guest"
    install_prebuilts "$PREFIX/guest"
elif [ "$SOURCE" -eq 1 ]; then
    # ---- SOURCE MODE: build mesa + virglrenderer, then fetch prebuilts ----
    build_from_source "$WORK" "$REPO_SRC"
    mkdir -p "$PREFIX/guest"
    install_prebuilts "$PREFIX/guest"
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
    # prebuilts (ANGLE, hwcomposer, surfaceflinger) — fetched from this tag
    # when present, otherwise install_prebuilts() falls back to this repo's
    # last release that ships them.
    if curl -fSL "$BASE_URL/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" \
            -o "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" 2>/dev/null; then
        info "prebuilts (ANGLE/hwcomposer/surfaceflinger) downloaded"
    else
        info "prebuilts not in $TAG — will use this repo's $PREBUILTS_FALLBACK_FILE fallback"
    fi

    # `sha256sum -c --ignore-missing` only skips entries whose file is absent —
    # it does NOT care whether the files we downloaded are listed at all. With a
    # SHA256SUMS that omits our tarballs it prints OK and exits 0, so unverified
    # bytes would get unpacked into /usr as root. Demand an entry per artifact.
    #
    # CI writes the list with `sha256sum ./*.tar.*`, so every name carries a
    # leading "./"; accept that form and normalise it away for the check.
    info "verifying checksums"
    SUMS="$WORK/SHA256SUMS"
    VERIFY_LINES="$WORK/.verify"
    : > "$VERIFY_LINES"
    for f in waydroid-nvidia-host-x86_64-${TAG}.tar.gz \
             waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz \
             $([ -f "$WORK/waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz" ] \
                   && echo "waydroid-nvidia-guest-prebuilts-${TAG}.tar.gz"); do
        # "<hash>[ ]<sp|*>[./]<name>" — exact basename, optional binary marker
        if ! grep -E "^[0-9a-fA-F]{64} [ *]?(\./)?${f//./\\.}\$" "$SUMS" \
                | sed -E "s#([ *])\./#\1#" >> "$VERIFY_LINES"; then
            die "SHA256SUMS has no entry for $f — refusing to install unverified binaries"
        fi
    done
    [ -s "$VERIFY_LINES" ] || die "no checksum lines collected — refusing to install"
    (cd "$WORK" && sha256sum -c --strict "$VERIFY_LINES") || die "SHA256 verification failed"
    ok "checksums verified ($(wc -l < "$VERIFY_LINES") artifacts)"

    info "installing host binaries to $PREFIX"
    mkdir -p "$PREFIX"
    tar -C "$PREFIX" -xf "$WORK/waydroid-nvidia-host-x86_64-${TAG}.tar.gz"

    info "installing guest binaries to $PREFIX/guest"
    mkdir -p "$PREFIX/guest"
    tar -C "$PREFIX/guest" -xf "$WORK/waydroid-nvidia-guest-android-x86_64-${TAG}.tar.gz"
    install_prebuilts "$PREFIX/guest"
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

# ---- pin the interpreter for installed waydroid entry points ----
# `#!/usr/bin/env python3` resolves through the *caller's* PATH, so a
# user-local python (Homebrew, pyenv, conda, ~/.local) shadows the system one
# and waydroid dies with "ModuleNotFoundError: No module named 'dbus'".
# Rewrite the shebang to an absolute interpreter that really has the deps.
info "selecting python interpreter for waydroid"
PYBIN=""
for cand in /usr/bin/python3 /usr/local/bin/python3; do
    [ -x "$cand" ] || continue
    if "$cand" -c 'import dbus, dbus.mainloop.glib, gi, gbinder' >/dev/null 2>&1; then
        PYBIN="$cand"
        break
    fi
done
if [ -z "$PYBIN" ]; then
    die "no system python3 with the dbus, gobject and gbinder bindings was found.
Install them for /usr/bin/python3 (python3-dbus + python3-gobject + python3-gbinder on
Debian/Fedora, python-dbus + python-gobject + python-gbinder on Arch) and re-run this
installer."
fi
ok "using $PYBIN ($("$PYBIN" -c 'import sys; print(".".join(map(str, sys.version_info[:3])))'))"

# Only ever rewrite the real script. /usr/bin/waydroid is a relative symlink to
# it (upstream Makefile), and that link is what puts /usr/lib/waydroid on
# sys.path[0] so `import tools` resolves. Editing through the link with `sed -i`
# would replace it with a copy under /usr/bin and break `import tools`.
sed -i "1s|^#!.*python3.*$|#!$PYBIN|" /usr/lib/waydroid/waydroid.py

# Repair the launcher if a previous in-place edit clobbered the symlink.
if [ ! -L /usr/bin/waydroid ]; then
    ln -sfn ../lib/waydroid/waydroid.py /usr/bin/waydroid
    info "restored /usr/bin/waydroid as a symlink to /usr/lib/waydroid/waydroid.py"
fi
ok "waydroid entry point pinned to $PYBIN"

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
if [ "$DISTRO" = "fedora" ]; then
cat <<'EOF'

Next steps (dnf/rpm family — Fedora, Nobara, Bazzite, RHEL, …):
  # Fedora's waydroid 1.6 package needs explicit OTA URLs:
  pkexec waydroid init -f -c https://ota.waydro.id/system -v https://ota.waydro.id/vendor
  pkexec waydroid-nvidia-setup        # configure the NVIDIA stack (--refresh <hz>)
  pkexec systemctl enable --now waydroid-container.service
  # re-login once (udev rule for /dev/udmabuf), then:
  waydroid session start
  # the patched session starts/stops wd-venus; do not enable that unit

Verify GPU acceleration:
  pkexec waydroid shell dumpsys SurfaceFlinger | grep GLES
  # should show: ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))

EOF
else
cat <<'EOF'

Next steps:
  waydroid init                     # download an Android image
  pkexec waydroid-nvidia-setup      # configure the NVIDIA stack (--refresh <hz>)
  pkexec systemctl enable --now waydroid-container.service
  # re-login once (udev rule for /dev/udmabuf), then:
  waydroid session start
  # the patched session starts/stops wd-venus; do not enable that unit

Verify GPU acceleration:
  pkexec waydroid shell dumpsys SurfaceFlinger | grep GLES
  # should show: ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))

EOF
fi
