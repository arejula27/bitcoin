#!/usr/bin/env python3
"""
Analyze ibd_block_spread.log (produced by ibd_block_spread.bt).

Each input line: "BLOCK <n> <tid>=<ns> <tid>=<ns> ..."

Per block, the "fair share" for each worker is 1/N (N = workers active in
that block, since shares always sum to 100%). For every worker we compute
its deviation from that fair share, in percentage points:

    dev_i = share_i - 1/N   (positive = overworked, negative = underworked)

Aggregating deviations *across many blocks* would converge to ~0 (law of
large numbers -- the same trap that made the global CV misleading in
earlier bench_bitcoin experiments, since a worker that overworks one block
tends to underwork another). Instead we pool every worker's per-block
deviation (all N of them, not just the busiest/laziest) within each
--bucket-size window and report the *distribution* of that pooled set, so
the full shape (not just two extreme points) stays visible.

Streams the file line by line so it can be run against a log that is
still being appended to (tail -f style) or against a finished one.
"""
import sys
import argparse


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * (p / 100)
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def parse_line(line):
    parts = line.split()
    if len(parts) < 3 or parts[0] != "BLOCK":
        return None
    block_n = int(parts[1])
    values = []
    for tok in parts[2:]:
        if "=" not in tok:
            continue
        _, ns = tok.split("=", 1)
        try:
            values.append(int(ns))
        except ValueError:
            continue
    return block_n, values


def print_rank_table(rank_sums, rank_sqsums, rank_counts, mean_share_pct):
    # Rank 1 = busiest worker of the block, rank N = laziest -- sorted by
    # execution time, not by thread id. Averaging *by rank* across many
    # blocks does NOT converge to the ideal mean like averaging by thread id
    # does: there is always a "busiest" and a "laziest" in every block, so
    # the order statistics carry a real, stable structural signal instead
    # of washing out via the law of large numbers.
    print("  Reparto medio por rango de ocupacion (rank 1 = mas ocupado del bloque):")
    max_rank = max(rank_counts.keys())
    for rank in range(1, max_rank + 1):
        count = rank_counts.get(rank, 0)
        if count == 0:
            continue
        mean = rank_sums[rank] / count
        var = max(rank_sqsums[rank] / count - mean * mean, 0.0)
        std = var ** 0.5
        dev = mean - mean_share_pct
        print(f"    rank {rank:2d}: media={mean:5.1f}%  (vs ideal {mean_share_pct:.1f}%: "
              f"{dev:+.1f}pp)  std={std:4.1f}pp  n={count}")


def print_histogram(label, vals_pp, n, edges):
    print(f"  Histograma ({label}, puntos porcentuales sobre la media ideal):")
    buckets = [0] * (len(edges) - 1)
    for v in vals_pp:
        for i in range(len(edges) - 1):
            hi_inclusive = (i == len(edges) - 2)
            if edges[i] <= v < edges[i + 1] or (hi_inclusive and v >= edges[i]):
                buckets[i] += 1
                break
        else:
            if v < edges[0]:
                buckets[0] += 1
    for i, count in enumerate(buckets):
        lo, hi = edges[i], edges[i + 1]
        bar = "#" * int(count / max(n, 1) * 60)
        pct = count / n * 100
        print(f"    {lo:+4d}..{hi:+4d}pp: {count:7d} ({pct:5.1f}%)  {bar}")


def report_bucket(bucket_start, bucket_end, all_devs, mean_share_pct,
                   rank_sums, rank_sqsums, rank_counts):
    n = len(all_devs)
    if n == 0:
        print(f"\n=== Blocks {bucket_start}-{bucket_end} (0 blocks) === (no data)")
        return

    n_blocks = n  # not exactly right if n_workers varies, but close enough for the header
    abs_devs = sorted(abs(d) for d in all_devs)
    p50 = percentile(abs_devs, 50)
    p90 = percentile(abs_devs, 90)
    p95 = percentile(abs_devs, 95)
    p99 = percentile(abs_devs, 99)
    pmax = abs_devs[-1]

    print(f"\n=== Blocks {bucket_start}-{bucket_end} (~{n_blocks} worker-block samples) ===")
    print(f"  Media ideal por worker (reparto perfecto): {mean_share_pct:.1f}%")

    print_rank_table(rank_sums, rank_sqsums, rank_counts, mean_share_pct)

    print(f"  |desviacion| de cada worker en cada bloque respecto a la media ideal:")
    print(f"    p50={p50:.1f}pp  p90={p90:.1f}pp  p95={p95:.1f}pp  p99={p99:.1f}pp  max={pmax:.1f}pp")

    over_20pp = sum(1 for d in all_devs if abs(d) > 20)
    print(f"  muestras worker-bloque con |desviacion| > 20pp: {over_20pp} ({over_20pp/n*100:.1f}%)")

    edges = [-100, -70, -50, -30, -20, -10, -5, 0, 5, 10, 20, 30, 50, 70, 100]
    print_histogram("todos los workers, todos los bloques", all_devs, n, edges)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile", help="ibd_block_spread.log path, or '-' for stdin")
    ap.add_argument("--bucket-size", type=int, default=100_000,
                     help="blocks per report bucket (default 100000)")
    args = ap.parse_args()

    src = sys.stdin if args.logfile == "-" else open(args.logfile)

    bucket_all_devs = []
    bucket_rank_sums = {}
    bucket_rank_sqsums = {}
    bucket_rank_counts = {}
    bucket_start = None
    blocks_in_bucket = 0
    total_blocks = 0
    last_mean_share_pct = 0.0

    def reset_bucket_accumulators():
        bucket_rank_sums.clear()
        bucket_rank_sqsums.clear()
        bucket_rank_counts.clear()

    with src:
        for line in src:
            parsed = parse_line(line)
            if parsed is None:
                continue
            block_n, values = parsed
            n_workers = len(values)
            if n_workers < 2 or sum(values) == 0:
                # need at least 2 workers with recorded time to compute a share
                continue

            total = sum(values)
            shares = [v / total for v in values]
            fair_share = 1.0 / n_workers  # shares always sum to 1, so mean == fair_share
            last_mean_share_pct = fair_share * 100

            if bucket_start is None:
                bucket_start = block_n

            for s in shares:
                bucket_all_devs.append((s - fair_share) * 100)

            # rank 1 = busiest (largest share) .. rank N = laziest (smallest share)
            for rank, s in enumerate(sorted(shares, reverse=True), start=1):
                pct = s * 100
                bucket_rank_sums[rank] = bucket_rank_sums.get(rank, 0.0) + pct
                bucket_rank_sqsums[rank] = bucket_rank_sqsums.get(rank, 0.0) + pct * pct
                bucket_rank_counts[rank] = bucket_rank_counts.get(rank, 0) + 1

            blocks_in_bucket += 1
            total_blocks += 1

            if blocks_in_bucket >= args.bucket_size:
                report_bucket(bucket_start, block_n, bucket_all_devs, last_mean_share_pct,
                               bucket_rank_sums, bucket_rank_sqsums, bucket_rank_counts)
                bucket_all_devs = []
                reset_bucket_accumulators()
                blocks_in_bucket = 0
                bucket_start = None

    if bucket_all_devs:
        report_bucket(bucket_start, bucket_start + blocks_in_bucket - 1,
                       bucket_all_devs, last_mean_share_pct,
                       bucket_rank_sums, bucket_rank_sqsums, bucket_rank_counts)

    print(f"\nTotal bloques procesados: {total_blocks}")


if __name__ == "__main__":
    main()
