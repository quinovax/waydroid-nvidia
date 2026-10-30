#!/usr/bin/env bash
# DEPLOY-STATS.sh - deploy the instrumented HAL to both waydroid roots, with backup, atomic replace,
# verification and a one-command rollback.  (task-11 deliverable; run BY HAND as bench-operator)
#
# It installs the task-4 + task-8 + task-12 + task-13 build (every debug switch is in every file):
#   plain build             (guest ICD libEGL_nvidia.so.0)    -> glibc-shim root
#   -DNVHAL_SOURCE_RM build (guest ICD libvulkan_nouveau.so)  -> prolapse root
#
# Usage:
#   bash nvidia-prolapse/hal/DEPLOY-STATS.sh verify        # read-only: repo vs live sha256 + switch counts
#   bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy        # backup, atomic replace, verify
#   DRY_RUN=1 bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy    # print what would run, write nothing
#   bash nvidia-prolapse/hal/DEPLOY-STATS.sh rollback DIR  # restore all four files from backup DIR
#
# It never restarts the stack and never starts an app: after a deploy run the sanctioned dev/restart and
# the setprop lines it prints at the end yourself.  Env overrides: REPO, GLIBC_ROOT, PROLAPSE_ROOT,
# BACKUP_BASE, PKEXEC (empty = run as root without pkexec), DRY_RUN, EXPECT_XLATE, EXPECT_ASTC, EXPECT_MEMTYPES,
# EXPECT_MEMSTATS (debug.nvhal.exp.memstats is named three times: the property, its .window sibling and the
# banner line), EXPECT_MEMTYPE_TARGET, EXPECT_HOSTVIS_CACHED, EXPECT_PRESENT (the "alloc census @present" marker
# that tells a task-16 build from an older one).
set -euo pipefail

MODE=${1:-deploy}
REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
GLIBC_ROOT=${GLIBC_ROOT:-/var/lib/waydroid/nvidia-glibc-shim-staging}
PROLAPSE_ROOT=${PROLAPSE_ROOT:-/var/lib/waydroid/nvidia-prolapse-staging}
BACKUP_BASE=${BACKUP_BASE:-$REPO/artifacts/hal-backup-$(date +%Y%m%d-%H%M%S)}
PKEXEC=${PKEXEC-pkexec}
DRY_RUN=${DRY_RUN:-0}
EXPECT_XLATE=${EXPECT_XLATE:-3}
EXPECT_ASTC=${EXPECT_ASTC:-3}
EXPECT_MEMTYPES=${EXPECT_MEMTYPES:-1}
EXPECT_MEMSTATS=${EXPECT_MEMSTATS:-3}
EXPECT_MEMTYPE_TARGET=${EXPECT_MEMTYPE_TARGET:-3}
EXPECT_HOSTVIS_CACHED=${EXPECT_HOSTVIS_CACHED:-3}
EXPECT_PRESENT=${EXPECT_PRESENT:-1}
if [ -n "$PKEXEC" ] && [ "$(id -u)" = 0 ]; then PKEXEC=; fi

say()    { printf '%s\n' "$*"; }
die()    { printf 'FATAL: %s\n' "$*" >&2; exit 1; }
header() { printf '\n=== %s\n' "$*"; }
sha()    { local o; o=$(sha256sum "$1"); printf '%s' "${o%% *}"; }
size()   { stat -c%s "$1"; }
mtim()   { stat -c%y "$1" | cut -c1-19; }
census_present() { # file -> how many "alloc census @present" markers it carries (task-16 build check)
    strings -a "$1" | grep -c 'alloc census @present' || true
}
switches() { # file -> "<xlate> <astc> <memtypes> <memstats> <memtype_target> <hostvis_cached>"
    local x a m s t c
    x=$(strings -a "$1" | grep -c 'debug.nvhal.xlate_stats' || true)
    a=$(strings -a "$1" | grep -c 'debug.nvhal.astc_bytes' || true)
    m=$(strings -a "$1" | grep -c 'debug.nvhal.exp.memtypes' || true)
    s=$(strings -a "$1" | grep -c 'debug.nvhal.exp.memstats' || true)
    t=$(strings -a "$1" | grep -c 'debug.nvhal.exp.memtype_target' || true)
    c=$(strings -a "$1" | grep -c 'debug.nvhal.exp.hostvis_cached' || true)
    printf '%s %s %s %s %s %s' "${x:-0}" "${a:-0}" "${m:-0}" "${s:-0}" "${t:-0}" "${c:-0}"
}
as_root() { # the only place that writes: pkexec here, direct when already root
    if [ -n "$PKEXEC" ]; then "$PKEXEC" "$@"; else "$@"; fi
}

# label|repo artifact|live path
targets() {
    printf '%s|%s|%s\n' glibc-shim-lib64 \
        "$REPO/nvidia-prolapse/hal/out/android/vulkan.nvidia.so" \
        "$GLIBC_ROOT/vendor_extra/lib64/hw/vulkan.nvidia.so"
    printf '%s|%s|%s\n' glibc-shim-lib32 \
        "$REPO/nvidia-prolapse/hal/out/android32/vulkan.nvidia.so" \
        "$GLIBC_ROOT/vendor_extra/lib/hw/vulkan.nvidia.so"
    printf '%s|%s|%s\n' prolapse-lib64 \
        "$REPO/build/source-native-integration/android/vulkan.nvidia.so" \
        "$PROLAPSE_ROOT/vendor_extra/lib64/hw/vulkan.nvidia.so"
    printf '%s|%s|%s\n' prolapse-lib32 \
        "$REPO/build/source-native-integration/android32/vulkan.nvidia.so" \
        "$PROLAPSE_ROOT/vendor_extra/lib/hw/vulkan.nvidia.so"
}

# Every repo artifact must exist and must really carry the instrumentation: deploying an uninstrumented
# (or half-rebuilt) .so would silently cost a whole measurement run.
preflight() {
    header "preflight (repo artifacts)"
    local label src dst x a m s t c n=0
    while IFS='|' read -r label src dst; do
        [ -f "$src" ] || die "repo artifact missing: $src (build first - see XLATE-STATS.md / ASTC-BYTES.md)"
        read -r x a m s t c <<<"$(switches "$src")"
        [ "$x" -ge 1 ] || die "$src has no debug.nvhal.xlate_stats string: not the instrumented build"
        [ "$a" -ge 1 ] || die "$src has no debug.nvhal.astc_bytes string: not the instrumented build"
        [ "$m" -ge 1 ] || die "$src has no debug.nvhal.exp.memtypes string: not the task-12 build"
        [ "$s" -ge 1 ] || die "$src has no debug.nvhal.exp.memstats string: not the task-13 build"
        [ "$t" -ge 1 ] || die "$src has no debug.nvhal.exp.memtype_target string: not the task-14 build"
        [ "$c" -ge 1 ] || die "$src has no debug.nvhal.exp.hostvis_cached string: not the task-14 build"
        [ "$(census_present "$src")" -ge 1 ] || die "$src has no 'alloc census @present' marker: not the task-16 build"
        [ -f "$dst" ] || die "live file missing: $dst"
        say "ok  $label  $(size "$src") B  sha256=$(sha "$src")  xlate=$x astc=$a memtypes=$m memstats=$s memtype_target=$t hostvis_cached=$c"
        n=$((n + 1))
    done < <(targets)
    [ "$n" = 4 ] || die "expected 4 targets, saw $n"
    if [ ! -w "$(dirname "$(targets | head -1 | cut -d'|' -f3)")" ]; then
        say "note: live dirs are not writable by $(id -un) -> using PKEXEC=${PKEXEC:-<none>}"
    fi
}

show_state() { # the live files exactly as they are now, before anything is touched
    header "live state before"
    local label src dst
    while IFS='|' read -r label src dst; do
        say "$label"
        say "  $dst"
        say "  $(size "$dst") B  mtime=$(mtim "$dst")  sha256=$(sha "$dst")"
    done < <(targets)
}

cmd_verify() {
    header "verify: repo artifacts vs live files (read-only)"
    local label src dst ss ls x a m s t c st bad=0 same=0 different=0
    while IFS='|' read -r label src dst; do
        [ -f "$src" ] || { say "MISSING repo: $src"; bad=$((bad + 1)); continue; }
        [ -f "$dst" ] || { say "MISSING live: $dst"; bad=$((bad + 1)); continue; }
        ss=$(sha "$src"); ls=$(sha "$dst")
        read -r x a m s t c <<<"$(switches "$src")"
        if [ "$ss" = "$ls" ]; then st=SAME; else st=DIFFERENT; fi
        if [ "$st" = SAME ]; then same=$((same + 1)); else different=$((different + 1)); fi
        say "$label  => $st"
        say "  repo $(size "$src") B sha256=$ss (xlate=$x astc=$a memtypes=$m memstats=$s memtype_target=$t hostvis_cached=$c)"
        say "  live $(size "$dst") B sha256=$ls"
    done < <(targets)
    [ "$bad" = 0 ] || die "$bad target(s) missing"
    say ""
    if [ "$same" = 4 ]; then
        say "same=4 different=0 -> the deployed HAL is exactly these repo artifacts."
    else
        say "same=$same different=$different -> the live copies are stale or uninstrumented; deploy to sync them."
    fi
}

cmd_deploy() {
    preflight
    show_state
    if [ "$DRY_RUN" = 1 ]; then
        header "dry run: these are the writes that would happen (nothing is written)"
        local label src dst
        say "mkdir -p $BACKUP_BASE/<label>/"
        while IFS='|' read -r label src dst; do
            say "cp -a $dst $BACKUP_BASE/$label/vulkan.nvidia.so"
            say "${PKEXEC:+$PKEXEC }/usr/bin/install -m 0644 $src $dst.new"
            say "${PKEXEC:+$PKEXEC }/usr/bin/mv -f $dst.new $dst"
        done < <(targets)
        say "dry run complete: no file was created, moved or replaced."
        return 0
    fi

    header "backup -> $BACKUP_BASE"
    mkdir -p "$BACKUP_BASE"
    : >"$BACKUP_BASE/TARGETS.tsv"
    local label src dst bsha lsha x a m s t c
    while IFS='|' read -r label src dst; do
        read -r x a m s t c <<<"$(switches "$src")"
        mkdir -p "$BACKUP_BASE/$label"
        lsha=$(sha "$dst")
        cp -a "$dst" "$BACKUP_BASE/$label/vulkan.nvidia.so"
        bsha=$(sha "$BACKUP_BASE/$label/vulkan.nvidia.so")
        [ "$bsha" = "$lsha" ] || die "backup of $dst is corrupt ($bsha != $lsha)"
        printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$dst" "$BACKUP_BASE/$label/vulkan.nvidia.so" "$lsha" "$(sha "$src")" \
            >>"$BACKUP_BASE/TARGETS.tsv"
        say "backed up $label  $(size "$dst") B  sha256=$lsha  (verified)"
    done < <(targets)
    cp -a "$0" "$BACKUP_BASE/DEPLOY-STATS.sh" 2>/dev/null || true

    # rollback lives inside the backup dir, so restoring does not depend on this script or on git
    cat >"$BACKUP_BASE/ROLLBACK.sh" <<'RB'
#!/usr/bin/env bash
# restore every file recorded in TARGETS.tsv (run as yourself; pkexec does the write)
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
if [ "$(id -u)" = 0 ]; then PKEXEC=""; else PKEXEC="pkexec"; fi
sha() { sha256sum "$1" | cut -d' ' -f1; }
while IFS=$'\t' read -r label live backup want _new; do
    [ -f "$backup" ] || { echo "FATAL: backup missing: $backup" >&2; exit 1; }
    if [ -n "$PKEXEC" ]; then "$PKEXEC" /usr/bin/install -m 0644 "$backup" "$live.new"; else /usr/bin/install -m 0644 "$backup" "$live.new"; fi
    got=$(sha "$live.new")
    [ "$got" = "$want" ] || { echo "FATAL: staged $live.new has $got, want $want" >&2; exit 1; }
    if [ -n "$PKEXEC" ]; then "$PKEXEC" /usr/bin/mv -f "$live.new" "$live"; else /usr/bin/mv -f "$live.new" "$live"; fi
    got=$(sha "$live")
    [ "$got" = "$want" ] || { echo "FATAL: $live is $got after restore, want $want" >&2; exit 1; }
    echo "restored $label  $(stat -c%s "$live") B  sha256=$got"
done <"$DIR/TARGETS.tsv"
echo "rollback complete: all four files are byte-identical to the pre-deploy state."
RB
    printf 'backup dir: %s\nrollback:   bash %s/ROLLBACK.sh   (or: bash %s rollback %s)\n' \
        "$BACKUP_BASE" "$BACKUP_BASE" "$0" "$BACKUP_BASE"

    on_err() {
        trap - ERR
        printf '\nFATAL: deploy failed after the backup was taken.\nRestore everything with:\n  bash %s/ROLLBACK.sh\n' "$BACKUP_BASE" >&2
    }
    trap on_err ERR

    header "install (install to .new, verify, then atomic mv -f)"
    while IFS='|' read -r label src dst; do
        read -r x a m s t c <<<"$(switches "$src")"
        before=$(sha "$dst")
        as_root /usr/bin/install -m 0644 "$src" "$dst.new"
        staged=$(sha "$dst.new")
        want=$(sha "$src")
        [ "$staged" = "$want" ] || die "staged $dst.new has $staged, want $want (nothing replaced)"
        as_root /usr/bin/mv -f "$dst.new" "$dst"
        after=$(sha "$dst")
        [ "$after" = "$want" ] || die "$dst is $after after replace, want $want"
        say "$label"
        say "  before sha256=$before"
        say "  after  sha256=$after   xlate=$x astc=$a memtypes=$m memstats=$s memtype_target=$t hostvis_cached=$c"
    done < <(targets)
    trap - ERR

    header "post-deploy verification"
    local bad=0
    while IFS='|' read -r label src dst; do
        read -r x a m s t c <<<"$(switches "$dst")"
        p=$(census_present "$dst")
        ss=$(sha "$src"); ls=$(sha "$dst")
        if [ "$ss" = "$ls" ] && [ "$x" = "$EXPECT_XLATE" ] && [ "$a" = "$EXPECT_ASTC" ] && [ "$m" = "$EXPECT_MEMTYPES" ] && [ "$s" = "$EXPECT_MEMSTATS" ] && [ "$t" = "$EXPECT_MEMTYPE_TARGET" ] && [ "$c" = "$EXPECT_HOSTVIS_CACHED" ] && [ "$p" = "$EXPECT_PRESENT" ]; then
            say "ok  $label  sha256=$ls  xlate=$x astc=$a memtypes=$m memstats=$s memtype_target=$t hostvis_cached=$c census_present=$p"
        else
            say "BAD $label  sha256=$ls (repo $ss)  xlate=$x astc=$a memtypes=$m memstats=$s memtype_target=$t hostvis_cached=$c census_present=$p"
            bad=$((bad + 1))
        fi
    done < <(targets)
    [ "$bad" = 0 ] || die "$bad target(s) did not verify - roll back: bash $BACKUP_BASE/ROLLBACK.sh"

    header "next steps (not done by this script)"
    say "1. restart the stack with the sanctioned dev/restart (bench-operator only)"
    say "2. in the guest, before starting the app:"
    say "     setprop debug.nvhal.xlate_stats 1     # task-4: counts + per-call timing"
    say "     setprop debug.nvhal.astc_bytes 1      # task-8: compressed-format VRAM"
    say "     setprop debug.nvhal.exp.memtypes 1    # task-12: print the memory type/heap table"
    say "     setprop debug.nvhal.exp.memstats 1    # task-13/16: allocation census"
    say "       -> a throttled 'alloc census @present #<n> t=<s>' line on the present path (survives a benchmark"
    say "          that never destroys its VkDevice) plus the '@final' line at vkDestroyDevice"
    say "     setprop debug.nvhal.exp.memstats.window 5  # optional: seconds between @present lines (default 5, min 0.1)"
    say "   task-14 Run B only (one mechanism at a time; memtype_target beats hostvis_to_bar):"
    say "     setprop debug.nvhal.exp.memtype_target <i>  # every type without DEVICE_LOCAL -> type <i>"
    say "     setprop debug.nvhal.exp.hostvis_cached 1    # HV|COHERENT -> same-heap HOST_CACHED"
    say "   read the src->dst allocation count/bytes summary in the final 'EXPERIMENT memory remap summary' line"
    say "   optional, task-13 (b): per-draw timestamps + DRAW-DIST per-render-pass distribution"
    say "     setprop debug.nvhal.gputime 2         # level 2 = one timestamp per draw in an inline render pass"
    say "3. smoke run of 3DMark Steel Nomad Light, judged against the known-good glibc-shim run:"
    say "     score ~196, 1.4578 fps, result zip > 10 KB (a 3.5-6 KB zip = the workload never ran)"
    say "   materially below 196 with no explanation -> roll back immediately:"
    say "     bash $BACKUP_BASE/ROLLBACK.sh"
    say "4. collect: logcat -s vulkan.nvidia | grep -E 'statistics ON|xlate stats\[|astc bytes\[|EXPERIMENT|alloc census|DRAW-DIST'"
    say ""
    say "rollback (restores all four files byte-for-byte):"
    say "  bash $BACKUP_BASE/ROLLBACK.sh"
    say "backup dir: $BACKUP_BASE"
}

cmd_rollback() {
    local dir=${2:-${BACKUP_DIR:-}}
    [ -n "$dir" ] || die "usage: $0 rollback <backup-dir>   (the dir printed by the deploy run)"
    [ -f "$dir/TARGETS.tsv" ] || die "$dir/TARGETS.tsv not found"
    header "rollback from $dir"
    local label live backup want got
    while IFS=$'\t' read -r label live backup want _new; do
        [ -f "$backup" ] || die "backup missing: $backup"
        as_root /usr/bin/install -m 0644 "$backup" "$live.new"
        got=$(sha "$live.new")
        [ "$got" = "$want" ] || die "staged $live.new has $got, want $want"
        as_root /usr/bin/mv -f "$live.new" "$live"
        got=$(sha "$live")
        [ "$got" = "$want" ] || die "$live is $got after restore, want $want"
        say "restored $label  $(size "$live") B  sha256=$got"
    done <"$dir/TARGETS.tsv"
    say ""
    say "all files restored; run dev/restart (bench-operator) to put them in use."
}

case "$MODE" in
    verify) cmd_verify ;;
    deploy) cmd_deploy ;;
    rollback) cmd_rollback "$@" ;;
    *) die "unknown mode '$MODE' (use: verify | deploy | rollback DIR)" ;;
esac
