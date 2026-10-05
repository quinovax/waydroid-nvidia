#!/bin/sh
# repo-hygiene — hermetic guard against the two ways this repository has
# already blown up. Needs only git + coreutils: no GPU, no guest, no network.
#
#   1. Checksums pinned in a tracked file. A hash copied into build.yml or
#      install-from-release.sh goes stale the moment a release asset is
#      re-uploaded, and it fails as an opaque `sha256sum -c` that takes the
#      whole release job down (it did twice — v0.1.2 never got created).
#      Expected hashes must be read from the SHA256SUMS published with the
#      release, so they can never drift out of sync with the asset.
#
#   2. Large binaries committed into history. 92 MiB of AUR makepkg output
#      under packaging/aur/ was committed upstream and is still carried by
#      every clone; `git rm` removed it from the tree but not from history.
#      A blob that lands in history stays in every future clone forever.
#
# Exit: 0 = clean, 1 = regression found.
cd "$(dirname "$0")/.." || exit 1

fail=0
fail_msg() { printf 'FAIL: %s\n' "$1"; fail=1; }
ok_msg()   { printf '  ok: %s\n' "$1"; }

# ---- 1. no literal sha256 pinned in the release/install path ----------------
# Word-bounded so the regexes those files legitimately use to *parse* a
# SHA256SUMS line ("^[0-9a-fA-F]{64}") are not mistaken for a pinned hash.
pinned=0
for f in .github/workflows/build.yml packaging/install-from-release.sh; do
    [ -f "$f" ] || continue
    hits=$(grep -nE '(^|[^0-9A-Za-z])[0-9a-fA-F]{64}([^0-9A-Za-z]|$)' "$f" || true)
    if [ -n "$hits" ]; then
        fail_msg "$f pins a literal sha256 — read it from the release's SHA256SUMS"
        printf '%s\n' "$hits" | sed 's/^/       /'
        pinned=1
    fi
done
[ "$pinned" = 0 ] && ok_msg "no pinned checksums in build.yml / install-from-release.sh"

# ---- 2. no large blobs, no release artifacts, no packaging/aur --------------
if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "  (not a git checkout — skipped the tracked-file checks)"
    exit "$fail"
fi

big=$(git ls-tree -r -l HEAD | awk '$4 ~ /^[0-9]+$/ && $4 > 1048576 { printf "       %s bytes  %s\n", $4, $5 }')
if [ -n "$big" ]; then
    fail_msg "tracked files over 1 MiB (they stay in history forever once committed):"
    printf '%s\n' "$big"
else
    ok_msg "no tracked file over 1 MiB"
fi

archives=$(git ls-tree -r --name-only HEAD | grep -E '\.(tar\.gz|tar\.zst|tar\.xz|tar\.bz2|tgz|zip|apk)$' || true)
if [ -n "$archives" ]; then
    fail_msg "release/packaging archives are tracked — they belong on a release, not in git:"
    printf '%s\n' "$archives" | sed 's/^/       /'
else
    ok_msg "no release archives tracked"
fi

if git ls-tree -r --name-only HEAD | grep -q '^packaging/aur/'; then
    fail_msg "packaging/aur/ is back — that subtree is what put 92 MiB into history"
else
    ok_msg "packaging/aur/ absent"
fi

[ "$fail" = 0 ] && echo "PASS: repo hygiene"
exit "$fail"
