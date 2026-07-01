#!/usr/bin/env bash
# Measure per-block spread using perf probes on Chainstate::ConnectBlock
# entry/exit as block boundaries, combined with sched:sched_switch.
#
# Usage: sudo ./scripts/perf_connectblock_spread.sh [duration_seconds]
#
# Prerequisites (run once as root before this script):
#   MANGLED="_ZN10Chainstate12ConnectBlockERK6CBlockR20BlockValidationStateP11CBlockIndexR15CCoinsViewCacheb"
#   sudo perf probe -x ./build/bin/bench_bitcoin --add "connect_block_start=${MANGLED}"

set -euo pipefail

DURATION=${1:-10}
BINARY="./build/bin/bench_bitcoin"
PERF_DATA="perf_connectblock.data"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root (sudo $0)" >&2
    exit 1
fi

# Check probes exist
if ! perf probe --list 2>/dev/null | grep -q "connect_block_start"; then
    echo "Probe not registered. Run first:"
    MANGLED="_ZN10Chainstate12ConnectBlockERK6CBlockR20BlockValidationStateP11CBlockIndexR15CCoinsViewCacheb"
    echo "  sudo perf probe -x $BINARY --add \"connect_block_start=\${MANGLED}\""
    exit 1
fi

echo "=== Launching benchmark (10 workers, ${DURATION}s) ==="
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
    "$BINARY" -filter=ConnectBlockMixedEcdsaSchnorr -par=10 \
    >/dev/null 2>&1 &
BENCH_PID=$!
echo "Benchmark PID: $BENCH_PID"

# Wait for workers to spin up (b-scriptch threads appear after setup)
sleep 2

echo "=== Recording perf events for ${DURATION}s ==="
perf record -p "$BENCH_PID" \
    -e "probe_bench_bitcoin:connect_block_start" \
    -e "sched:sched_switch" \
    -o "$PERF_DATA" \
    -- sleep "$DURATION"

chmod a+r "$PERF_DATA"
kill "$BENCH_PID" 2>/dev/null || true
wait "$BENCH_PID" 2>/dev/null || true

echo "=== Parsing events ==="
# Dump raw events: timestamp + event name + comm
perf script -i "$PERF_DATA" --fields time,event,comm 2>/dev/null \
    | python3 "$(dirname "$0")/parse_connectblock_spread.py"
