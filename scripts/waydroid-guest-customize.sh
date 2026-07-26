#!/usr/bin/env bash
# scripts/waydroid-guest-customize.sh — Optional container customization script
#
# Applies common Waydroid guest tweaks that are NOT part of waydroid-nvidia
# core functionality but are useful for gaming/anti-detect/font-fix use cases.
#
# Requirements: waydroid running + wsu (WaydroidSU) installed + root for overlays
#
# Each feature is independently toggleable. Run with --help for options.
set -euo pipefail

red()   { printf '\033[1;31m%s\033[0m\n' "$*"; }
green() { printf '\033[1;32m%s\033[0m\n' "$*"; }
blue()  { printf '\033[1;34m%s\033[0m\n' "$*"; }
warn()  { printf '\033[1;33mWARN:\033[0m %s\n' "$*"; }
die()   { red "FATAL: $*"; exit 1; }

ADB_HOST=""
adb_connect() {
  ADB_HOST=$(waydroid status 2>/dev/null | awk -F': *' '/IP address/{gsub(/^[ \t]+|[ \t]+$/,"",$2); print $2}')
  [[ -n "${ADB_HOST:-}" && "$ADB_HOST" != "UNKNOWN" ]] || die "waydroid not running"
  adb connect "$ADB_HOST:5555" >/dev/null 2>&1 || true
  adb -s "$ADB_HOST:5555" shell true 2>/dev/null || die "adb not responding"
}
ADB() { adb -s "$ADB_HOST:5555" "$@"; }

OVERLAY_SYSTEM="/var/lib/waydroid/overlay/system"
CFG="/var/lib/waydroid/waydroid.cfg"
need_root() { [[ "$(id -u)" -eq 0 ]] || die "must run as root (sudo)"; }

# ── Magisk + Zygisk + Shamiko ──
install_magisk() {
  blue "== Magisk + Zygisk + Shamiko"
  command -v wsu >/dev/null 2>&1 || die "wsu not found — install https://github.com/mistrmochov/WaydroidSU"
  local s; s=$(wsu status 2>&1 || true)
  if echo "$s" | grep -q "Installed:"; then green "  Magisk already installed"
  else green "  Installing Magisk..."; wsu install 2>&1; fi
  green "  Enabling Zygisk..."
  wsu zygisk enable 2>&1 || true
  green "  Enabling denylist..."
  wsu denylist enable 2>&1 || true
  if wsu module list 2>&1 | grep -q shamiko; then
    green "  Shamiko installed"
  else
    warn "  Shamiko not installed — download from https://github.com/LSPosed/LSPosed.github.io/releases"
    warn "    sudo wsu module install /path/to/Shamiko.zip"
    warn "  Then: sudo wsu denylist disable   (Shamiko handles hiding)"
  fi
  green "  Done: $(wsu status 2>&1 | head -2)"
}

# ── WebView GL ──
webview_gl() {
  blue "== WebView: disable Vulkan draw functor, force GL"
  adb_connect
  printf '_ --disable-features=WebViewDrawFunctorVulkan,WebViewVulkan,Vulkan,DefaultANGLEVulkan,VulkanFromANGLE\n' > /tmp/wd-wv-flags
  ADB push /tmp/wd-wv-flags /data/local/tmp/webview-command-line
  ADB push /tmp/wd-wv-flags /data/local/tmp/chrome-command-line
  ADB shell chmod 644 /data/local/tmp/webview-command-line /data/local/tmp/chrome-command-line
  ADB shell setprop debug.hwui.renderer skiagl 2>/dev/null || true
  ADB shell am force-stop com.google.android.webview 2>/dev/null || true
  python3 - "$CFG" <<'PY'
import configparser, sys
from pathlib import Path
cfg=Path(sys.argv[1]); cp=configparser.ConfigParser(); cp.optionxform=str; cp.read(cfg)
p=cp.setdefault('properties',{})
p['debug.hwui.renderer']='skiagl'
p['debug.renderengine.backend']='skiaglthreaded'
with cfg.open('w') as f: cp.write(f)
PY
  green "  HWUI=skiagl, WebView Vulkan draw functor disabled"
}

# ── Mouse/cursor fix ──
mouse_fix() {
  blue "== Mouse/cursor: enable relative motion for games"
  adb_connect
  ADB shell settings put system pointer_speed -4 2>/dev/null || true
  ADB shell setprop persist.waydroid.cursor_on_subsurface false 2>/dev/null || true
  ADB shell setprop persist.waydroid.fake_touch 1 2>/dev/null || true
  python3 - "$CFG" <<'PY'
import configparser, sys
from pathlib import Path
cfg=Path(sys.argv[1]); cp=configparser.ConfigParser(); cp.optionxform=str; cp.read(cfg)
p=cp.setdefault('properties',{})
p['persist.waydroid.cursor_on_subsurface']='false'
p['persist.waydroid.fake_touch']='1'
with cfg.open('w') as f: cp.write(f)
PY
  green "  cursor_on_subsurface=false, fake_touch=1"
}

# ── Device identity spoof ──
device_spoof() {
  blue "== Device identity spoof"
  need_root
  local MODEL="${SPOOF_MODEL:-VOG-AL10}"
  local BRAND="${SPOOF_BRAND:-HUAWEI}"
  local DEVICE="${SPOOF_DEVICE:-HWVOG}"
  green "  Spoofing as: $BRAND $MODEL ($DEVICE)"
  python3 - "$CFG" "$MODEL" "$BRAND" "$DEVICE" <<'PY'
import configparser, sys
from pathlib import Path
cfg=Path(sys.argv[1]); model=sys.argv[2]; brand=sys.argv[3]; device=sys.argv[4]
cp=configparser.ConfigParser(); cp.optionxform=str; cp.read(cfg)
p=cp.setdefault('properties',{})
for k,v in {
    'ro.product.brand':brand,'ro.product.manufacturer':brand,
    'ro.product.model':model,'ro.product.device':device,'ro.product.name':model,
    'ro.system.build.product':model,'ro.system.build.flavor':f'{device}-user',
    'ro.build.fingerprint':f'{brand}/{model}/{device}:10/{brand}{model}/10.1.0.162C00:user/release-keys',
    'ro.system.build.description':f'{model}-user 10 {brand}{model} release-keys',
    'ro.build.display.id':f'{model} 10.1.0.162(C00E160R1P8)',
    'ro.build.tags':'release-keys','ro.debuggable':'0',
    'ro.product.system.brand':brand,'ro.product.system.manufacturer':brand,
    'ro.product.system.model':model,'ro.product.system.device':device,'ro.product.system.name':model,
    'ro.product.vendor.brand':brand,'ro.product.vendor.manufacturer':brand,
    'ro.product.vendor.model':model,'ro.product.vendor.device':device,'ro.product.vendor.name':model,
    'ro.product.odm.brand':brand,'ro.product.odm.manufacturer':brand,
    'ro.product.odm.model':model,'ro.product.odm.device':device,'ro.product.odm.name':model,
    'ro.product.system_ext.brand':brand,'ro.product.system_ext.manufacturer':brand,
    'ro.product.system_ext.model':model,'ro.product.system_ext.device':device,'ro.product.system_ext.name':model,
}.items():
    p[k]=v
p.setdefault('ro.hardware','unknown')
p.setdefault('ro.board.platform','waydroid')
with cfg.open('w') as f: cp.write(f)
PY
  green "  Identity spoofed as $BRAND $MODEL"
}

# ── Settings tweaks ──
settings_tweaks() {
  blue "== System settings tweaks"
  adb_connect
  ADB shell settings put global development_settings_enabled 0 2>/dev/null || true
  ADB shell settings put secure install_non_market_apps 1 2>/dev/null || true
  ADB shell settings put global package_verifier_enable 0 2>/dev/null || true
  ADB shell settings put global verifier_verify_adb_installs 0 2>/dev/null || true
  ADB shell settings put global adb_enabled 1 2>/dev/null || true
  ADB shell settings put system pointer_speed -4 2>/dev/null || true
  green "  Developer settings hidden, package verifier off, pointer speed -4"
}

# ── Main ──
usage() {
  cat <<EOF
Usage: sudo $0 [OPTIONS]

Optional container customizations for waydroid-nvidia.

Options:
  --all               Apply all features below
  --magisk            Install Magisk 30.1-Waydroid + Zygisk + Shamiko (requires wsu)
  --webview-gl        Force WebView/Chrome off Vulkan draw functor, use GL
  --mouse-fix         Enable relative mouse motion for games (fake_touch=1)
  --device-spoof      Spoof device as HUAWEI VOG-AL10 (override with SPOOF_MODEL env)
  --settings-tweaks   Disable dev settings, package verifier, set pointer speed
  --help              Show this help

Environment variables (for --device-spoof):
  SPOOF_MODEL   Device model   (default: VOG-AL10)
  SPOOF_BRAND   Device brand   (default: HUAWEI)
  SPOOF_DEVICE  Device codename (default: HWVOG)

Examples:
  sudo $0 --all
  SPOOF_MODEL=SM-S908B SPOOF_BRAND=samsung sudo $0 --device-spoof
EOF
}

[[ $# -eq 0 ]] && { usage; exit 0; }
DO_MAGISK=0; DO_WEBVIEW=0; DO_MOUSE=0; DO_SPOOF=0; DO_SETTINGS=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --magisk)         DO_MAGISK=1;;
    --webview-gl)     DO_WEBVIEW=1;;
    --mouse-fix)      DO_MOUSE=1;;
    --device-spoof)   DO_SPOOF=1;;
    --settings-tweaks) DO_SETTINGS=1;;
    -h|--help)        usage; exit 0;;
    *) die "unknown option: $1";;
  esac
  shift
done

[[ $DO_MAGISK -eq 1 ]]   && install_magisk
[[ $DO_WEBVIEW -eq 1 ]]  && webview_gl
[[ $DO_MOUSE -eq 1 ]]    && mouse_fix
[[ $DO_SPOOF -eq 1 ]]    && device_spoof
[[ $DO_SETTINGS -eq 1 ]] && settings_tweaks

green "All requested customizations applied."
echo "Restart waydroid session to pick up overlay changes:"
echo "  waydroid session stop && waydroid session start"
