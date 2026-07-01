#!/usr/bin/env python3
"""
Parse `perf script` output from a perf timechart recording with
probe_bench_bitcoin:connect_block_start added.

Expected input from:
  perf script -i perf.data --fields time,event,comm

Actual observed format (perf prepends comm regardless of --fields order):
  {comm}  {timestamp}:  {event}:
e.g.:
     b-scriptch.07 121392.507275:       sched:sched_switch:
             b-test 121392.526001:      probe_bench_bitcoin:connect_block_start:

Block window: [connect_block_start_N, connect_block_start_{N+1})
Spread = max(last_sched_switch per worker) - min(last_sched_switch per worker)
within each window.
"""

import sys
import statistics

WORKER_PREFIX = "b-scriptch"


def parse_line(line):
    """Return (comm, timestamp_s, event) or None."""
    line = line.strip()
    if not line:
        return None
    parts = line.split()
    if len(parts) < 3:
        return None
    comm = parts[0]
    # timestamp field ends with ':'
    try:
        ts = float(parts[1].rstrip(':'))
    except ValueError:
        return None
    # event name is the next token (also ends with ':')
    event = parts[2].rstrip(':')
    return comm, ts, event


def parse_events(lines):
    blocks = []
    block_start_ts = None
    worker_last_ts = {}

    for line in lines:
        parsed = parse_line(line)
        if parsed is None:
            continue
        comm, ts, event = parsed

        if "connect_block_start" in event and comm == "b-test":
            # Close previous block
            if block_start_ts is not None and len(worker_last_ts) >= 2:
                finishes = list(worker_last_ts.values())
                spread_ms = (max(finishes) - min(finishes)) * 1000
                dur_ms = (ts - block_start_ts) * 1000
                blocks.append({
                    "start": block_start_ts,
                    "duration_ms": dur_ms,
                    "spread_ms": spread_ms,
                    "n_workers": len(worker_last_ts),
                })
            block_start_ts = ts
            worker_last_ts = {}

        elif "sched_switch" in event and comm.startswith(WORKER_PREFIX):
            if block_start_ts is not None:
                worker_last_ts[comm] = ts

    return blocks


def pct(data, p):
    data = sorted(data)
    idx = int(len(data) * p / 100)
    return data[min(idx, len(data) - 1)]


def report(blocks):
    if not blocks:
        print("No blocks found.")
        print("Debug: check that 'b-test' and 'b-scriptch*' appear in perf output:")
        print("  perf script -i perf.data --fields time,event,comm | head -20")
        return

    usable = [b for b in blocks if b["n_workers"] >= 2]
    full   = [b for b in blocks if b["n_workers"] == 10]

    spreads   = [b["spread_ms"] for b in usable]
    durations = [b["duration_ms"] for b in usable]
    waste     = [s / d * 100 for s, d in zip(spreads, durations) if d > 0]

    print(f"\nBlocks analysed (>=2 workers): {len(usable)}")
    print(f"Blocks with all 10 workers:    {len(full)}")

    print("\n=== Block duration start-to-start (ms) ===")
    print(f"  Mean:    {statistics.mean(durations):.2f}")
    print(f"  Median:  {statistics.median(durations):.2f}")

    mean_s = statistics.mean(spreads)
    std_s  = statistics.stdev(spreads)
    print("\n=== Spread (max_finish - min_finish) per block (ms) ===")
    print(f"  Mean:    {mean_s:.2f}")
    print(f"  Median:  {statistics.median(spreads):.2f}")
    print(f"  Stddev:  {std_s:.2f}")
    print(f"  p95:     {pct(spreads, 95):.2f}")
    print(f"  p99:     {pct(spreads, 99):.2f}")
    print(f"  Max:     {max(spreads):.2f}")
    print(f"  CV:      {std_s / mean_s * 100:.1f}%")

    print("\n=== Waste ratio (spread / block_duration) ===")
    print(f"  Mean:    {statistics.mean(waste):.1f}%")
    print(f"  p95:     {pct(waste, 95):.1f}%")

    print("\n=== Spread distribution ===")
    buckets = [0, 2, 4, 6, 8, 10, 15, 20, 30]
    counts  = [0] * len(buckets)
    for s in spreads:
        for i in range(len(buckets) - 1, -1, -1):
            if s >= buckets[i]:
                counts[i] += 1
                break
    for i, lo in enumerate(buckets):
        hi    = buckets[i + 1] if i + 1 < len(buckets) else None
        label = f"{lo:3d}-{hi:3d}ms" if hi else f"{lo:3d}+   ms"
        bar   = "█" * int(counts[i] / len(spreads) * 40)
        print(f"  {label}: {counts[i]:4d} ({counts[i]/len(spreads)*100:5.1f}%)  {bar}")


if __name__ == "__main__":
    lines = sys.stdin.readlines()
    blocks = parse_events(lines)
    report(blocks)
