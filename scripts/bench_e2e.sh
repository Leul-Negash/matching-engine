#!/usr/bin/env bash
# End-to-end benchmark: ob_server + ob_loadgen over loopback TCP, for each
# persistence mode, unloaded (1 connection, 1 request in flight) and loaded
# (4 connections x 32 in flight). Set JOURNAL_DIR to benchmark another disk.
#
# usage: scripts/bench_e2e.sh [build-dir]
set -euo pipefail

BUILD=${1:-build/release}
PORT=${PORT:-19555}
# The journal goes under build/: /tmp is often tmpfs, where fsync is free and
# the numbers would be meaningless.
TMP=$(mktemp -d "${JOURNAL_DIR:-build}/bench_e2e.XXXXXX")
trap 'kill "$SERVER" 2>/dev/null || true; rm -rf "$TMP"' EXIT

run() {
    local label=$1; shift
    rm -f "$TMP/ob.journal"
    "$BUILD/ob_server" --symbol AAPL --port "$PORT" --max-rate 0 --log-level warn "$@" \
        </dev/null >/dev/null 2>"$TMP/server.log" &
    SERVER=$!
    for _ in $(seq 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
        sleep 0.1
    done

    echo "== $label"
    echo "-- unloaded: 1 connection, 1 in flight"
    "$BUILD/ob_loadgen" --port "$PORT" --connections 1 --window 1 --requests 20000 | sed -n '2,3p'
    echo "-- loaded: 4 connections, 32 in flight each"
    "$BUILD/ob_loadgen" --port "$PORT" --connections 4 --window 32 --requests 100000 | sed -n '2,3p'

    kill -INT "$SERVER"
    wait "$SERVER"
    echo
}

run "no journal"
run "journal (write per batch)" --journal "$TMP/ob.journal"
run "journal + fsync per batch" --journal "$TMP/ob.journal" --fsync
run "journal, busy-poll" --journal "$TMP/ob.journal" --busy-poll
