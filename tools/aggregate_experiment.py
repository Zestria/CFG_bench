"""Aggregate raw cfg_bench experiment results.

Each row in the input CSV is one measured run. For every source size and chunk
the script first averages the repeated runs of that chunk, then reports the
mean and median over all chunks. CFL_adv rows have source_size=0 and one chunk
per measured run, so the same code yields its mean and median over all runs.
"""

import argparse
import csv
import statistics
import sys
from collections import defaultdict


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="raw cfg_bench result CSV files")
    parser.add_argument("--output", default="summary.csv", help="summary CSV path (default: summary.csv)")
    parser.add_argument("--per-chunk", default="per_chunk.csv", help="per-chunk CSV path (default: per_chunk.csv)")
    parser.add_argument("--chunks", type=int, default=30, help="expected chunks per source size (default: 30)")
    parser.add_argument("--repeats", type=int, default=3, help="expected runs per chunk (default: 3)")
    parser.add_argument("--all-pairs-runs", type=int, default=10, help="expected CFL_adv runs (default: 10)")
    return parser.parse_args()


def load_rows(paths: list[str]) -> list[dict]:
    rows = []
    for path in paths:
        with open(path, newline="") as file:
            for row in csv.DictReader(file):
                if row.get("algo_name") == "algo_name":
                    continue
                if not row.get("time_s"):
                    continue

                rows.append(
                    {
                        "algo_name": row["algo_name"],
                        "cnf": row["cnf"],
                        "graph": row["graph"],
                        "source_size": int(row["source_size"]),
                        "chunk": int(row["chunk"]),
                        "time_s": float(row["time_s"]),
                        "result": int(row.get("result") or 0),
                        "max_memory_kb": int(row.get("max_memory_kb") or 0),
                    }
                )
    return rows


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def main() -> int:
    args = parse_args()
    rows = load_rows(args.inputs)
    if not rows:
        print("No rows with time_s found", file=sys.stderr)
        return 1

    # chunk key -> rows of that chunk
    chunk_groups: dict[tuple, list[dict]] = defaultdict(list)
    for row in rows:
        key = (row["algo_name"], row["cnf"], row["graph"], row["source_size"], row["chunk"])
        chunk_groups[key].append(row)

    per_chunk = []
    for (algo_name, cnf, graph, source_size, chunk), chunk_rows in chunk_groups.items():
        times = [row["time_s"] for row in chunk_rows]
        per_chunk.append(
            {
                "algo_name": algo_name,
                "cnf": cnf,
                "graph": graph,
                "source_size": source_size,
                "chunk": chunk,
                "n_runs": len(chunk_rows),
                "mean_time_s": mean(times),
                "median_time_s": statistics.median(times),
                "mean_result": mean([row["result"] for row in chunk_rows]),
                "mean_max_memory_kb": mean([row["max_memory_kb"] for row in chunk_rows]),
            }
        )

    per_chunk.sort(key=lambda row: (row["algo_name"], row["graph"], row["source_size"], row["chunk"]))

    # (algo, cnf, graph, source_size) -> per-chunk measurements
    experiment_groups: dict[tuple, list[dict]] = defaultdict(list)
    for row in per_chunk:
        key = (row["algo_name"], row["cnf"], row["graph"], row["source_size"])
        experiment_groups[key].append(row)

    summary = []
    warnings = 0
    for (algo_name, cnf, graph, source_size), chunks in experiment_groups.items():
        chunks.sort(key=lambda row: row["chunk"])
        times = [row["mean_time_s"] for row in chunks]
        expected_chunks = args.all_pairs_runs if source_size == 0 else args.chunks
        if len(chunks) != expected_chunks:
            print(
                f"Warning: {algo_name} {graph} |S|={source_size}: expected {expected_chunks} "
                f"measurements, found {len(chunks)}",
                file=sys.stderr,
            )
            warnings += 1
        if source_size > 0:
            for row in chunks:
                if row["n_runs"] != args.repeats:
                    print(
                        f"Warning: {algo_name} {graph} |S|={source_size} chunk={row['chunk']}: "
                        f"expected {args.repeats} runs, found {row['n_runs']}",
                        file=sys.stderr,
                    )
                    warnings += 1

        summary.append(
            {
                "algo_name": algo_name,
                "cnf": cnf,
                "graph": graph,
                "source_size": source_size,
                "n_measurements": len(chunks),
                "mean_time_s": mean(times),
                "median_time_s": statistics.median(times),
                "std_time_s": statistics.stdev(times) if len(times) > 1 else 0.0,
                "min_time_s": min(times),
                "max_time_s": max(times),
                "mean_result": mean([row["mean_result"] for row in chunks]),
                "mean_max_memory_kb": mean([row["mean_max_memory_kb"] for row in chunks]),
            }
        )

    summary.sort(key=lambda row: (row["algo_name"], row["graph"], row["source_size"]))

    with open(args.per_chunk, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(per_chunk[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(per_chunk)

    with open(args.output, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(summary[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary)

    print(f"Wrote {len(summary)} summary rows to {args.output} and {len(per_chunk)} chunk rows to {args.per_chunk}")
    if warnings:
        print(f"Warnings: {warnings}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
