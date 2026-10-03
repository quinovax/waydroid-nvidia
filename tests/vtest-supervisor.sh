#!/usr/bin/env bash
# vtest-supervisor.sh — regression test for the render-server supervisor.
#
# What it proves: when one client's renderer is unrecoverably gone, the forked
# client process exits with status 69 (VTEST_EXIT_RENDERER_LOST) and the MAIN
# server -- the systemd main PID, and the only process that can hand the stack a
# clean GPU -- exits with the same status instead of staying alive with nothing
# behind it.
#
# Why it matters: an NVIDIA class error (Xid 69) poisons the device for the
# whole process tree, so every later vkCreateDevice fails even in a freshly
# forked worker; only a new server process recovers. A main process that never
# exits makes the unit's Restart= useless, and the guest is left in a permanent
# SurfaceFlinger crash-loop with a black window (issue #11).
#
# How it triggers a renderer loss with no GPU and no guest: the first command a
# client must send is VCMD_CREATE_RENDERER, and sending it with an impossible
# length makes vtest_create_context() refuse. The server classifies that as a
# renderer-side failure, which is also what a poisoned device looks like to a
# reconnecting client (CREATE_RENDERER is fine, the context is not).
#
# Host-agnostic: runs any built virgl_test_server on a private socket in /tmp,
# never touches /run/waydroid-venus, and never needs a GPU (the renderer is only
# initialised once a context is created, which this test makes fail first).
#
# Usage: tests/vtest-supervisor.sh [path/to/virgl_test_server]

set -euo pipefail

EXPECT_STATUS=69

bin="${1:-}"
if [ -z "$bin" ]; then
    for cand in "$HOME/waydroid-nv/virglrenderer/build/vtest/virgl_test_server" \
                /usr/lib/waydroid-nvidia/virgl_test_server; do
        if [ -x "$cand" ]; then bin="$cand"; break; fi
    done
fi
[ -n "$bin" ] && [ -x "$bin" ] || { echo "usage: $0 <path/to/virgl_test_server>" >&2; exit 2; }

sock="$(mktemp -u /tmp/vtest-supervisor.XXXXXX.sock)"
log="$(mktemp /tmp/vtest-supervisor.XXXXXX.log)"

server=
cleanup() {
    [ -n "$server" ] && kill -9 "$server" 2>/dev/null || true
    rm -f "$sock" "$log"
}
trap cleanup EXIT

echo "server: $bin"
"$bin" --no-virgl --venus --multi-clients --socket-path "$sock" >"$log" 2>&1 &
server=$!

for _ in $(seq 1 50); do
    [ -S "$sock" ] && break
    sleep 0.1
done
if [ ! -S "$sock" ]; then
    echo "FAIL: server never listened on $sock"
    sed 's/^/  | /' "$log"
    exit 1
fi

# A client whose very first command cannot be served: 2000000 dwords is larger
# than the 1 MiB cap vtest_create_context() enforces.
python3 - "$sock" <<'PY'
import socket, struct, sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack("<II", 2000000, 8))  # length_dw, VCMD_CREATE_RENDERER
s.close()
PY

# The main server has to be gone (and reaped) within a few seconds.
deadline=$((SECONDS + 10))
while kill -0 "$server" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do
    sleep 0.1
done

if kill -0 "$server" 2>/dev/null; then
    echo "FAIL: main server still alive ${SECONDS}s after its renderer was lost"
    echo "      (systemd would never restart the unit -- issue #11)"
    sed 's/^/  | /' "$log"
    exit 1
fi

set +e
wait "$server"
status=$?
set -e
server=

if [ "$status" -ne "$EXPECT_STATUS" ]; then
    echo "FAIL: main server exited $status, expected $EXPECT_STATUS"
    sed 's/^/  | /' "$log"
    exit 1
fi

if ! grep -q "VTEST_CLIENT_ERROR_CONTEXT_FAILED" "$log"; then
    echo "FAIL: client did not report the renderer-side failure"
    sed 's/^/  | /' "$log"
    exit 1
fi

if ! grep -q "a client renderer was lost" "$log"; then
    echo "FAIL: main server did not log the escalation"
    sed 's/^/  | /' "$log"
    exit 1
fi

echo "PASS: client reported the loss, main server exited $status for a restart"
