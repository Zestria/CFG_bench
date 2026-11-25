"""Run Java Points-To comparisons with the standard CFG_bench timing path."""

import argparse
import csv
import hashlib
import json
import math
import os
import shlex
import subprocess
import tempfile
from functools import partial
from itertools import pairwise
from pathlib import Path
from statistics import median

from run_experiment import candidate_name

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/java-points-to"
BENCH = BUILD / "cfg_bench"
RSM_LIB = BUILD / "p1_full_clean.so"
CONFIG = ROOT / "configs/java_points_to_campaign.csv"
PILOT_QUERIES = 150
PILOT_WINS = 135
ROUNDS = 3
RSM_SIZE = 100
RSM_QUERIES = 385
CFL_ROUNDS = 30
# Previous one-sided binomial plan: alpha 5%, joint power 90% at p=0.85 and p=0.95.
CONFIRMATION_CUTOFFS = {
    "lower": {"queries": 474, "min_losses": 59},
    "upper": {"queries": 298, "min_wins": 277},
}


def file_record(path):
    resolved = Path(path).resolve()
    with resolved.open("rb") as stream:
        return {"path": str(resolved), "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def master_seed(value):
    number = int(value)
    if not 0 <= number <= 2**32 - 1:
        raise argparse.ArgumentTypeError("seed must be an unsigned 32-bit integer")
    return number


def read_config(path, columns):
    with path.open(newline="") as stream:
        rows = list(csv.reader(stream))
    if not rows or any(len(row) != columns or not all(row) for row in rows):
        raise ValueError(f"{path}: expected a nonempty, headerless {columns}-column CFG_bench config")
    names = [Path(row[0]).stem for row in rows]
    if len(set(names)) != len(names):
        raise ValueError(f"{path}: graph names must be unique")
    return rows


def write_csv(path, rows):
    with path.open("x", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)


def command(stage, argv):
    args = list(map(str, argv))
    print(shlex.join(args), flush=True)
    with (stage / "commands.log").open("a") as stream:
        print(shlex.join(args), file=stream)
    with (stage / "run.log").open("a") as log:
        print(shlex.join(args), file=log, flush=True)
        subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)


def stage_dir(run, prefix):
    directory = Path(tempfile.mkdtemp(prefix=f"{prefix}.", dir=run))
    print(f"Run directory: {directory}", flush=True)
    return directory


def read_pool(path):
    nodes = list(map(int, Path(path).read_text().split()))
    if not nodes or nodes[0] < 0 or any(a >= b for a, b in pairwise(nodes)):
        raise ValueError(f"{path}: expected nonempty, strictly increasing vertex IDs")
    return nodes


def prepare(args):
    graphs = read_config(args.config, 3)
    inputs = set()
    for graph, grammar, _ in graphs:
        inputs.update((Path(graph), Path(grammar), Path(grammar).with_suffix(".rsm")))
    receipt = {"graphs": graphs, "inputs": [file_record(path) for path in sorted(inputs)]}
    args.directory.mkdir(parents=True, exist_ok=True)
    print(f"Run directory: {args.directory}", flush=True)
    graph_config = args.directory / "graphs.csv"
    pool_dir = args.directory / "start_nodes"
    receipt_path = args.directory / "preparation.json"
    if receipt_path.exists():
        if json.loads(receipt_path.read_text()) != receipt:
            raise ValueError("the inputs for this campaign directory have changed")
    else:
        if pool_dir.exists() and any(pool_dir.iterdir()):
            raise ValueError("the campaign directory already contains pools without a preparation receipt")
        write_json(receipt_path, receipt)
    pool_dir.mkdir(exist_ok=True)
    if not graph_config.exists():
        write_csv(graph_config, graphs)
    elif read_config(graph_config, 3) != graphs:
        raise ValueError(f"{graph_config}: graph config differs from the preparation receipt")

    rows = []
    for graph, grammar, expected in graphs:
        pool = (args.reuse_candidates or pool_dir) / candidate_name(graph, grammar)
        if not pool.exists() and args.reuse_candidates is None:
            with tempfile.TemporaryDirectory(prefix=".extract-", dir=args.directory) as temporary:
                output = Path(temporary)
                config = output / "graph.csv"
                write_csv(config, [(graph, grammar, expected)])
                command(args.directory, [BUILD / "pairs_extractor", "-c", config, "-o", output, "--start-only"])
                staged = output / pool.name
                read_pool(staged)
                staged.replace(pool)
        count = len(read_pool(pool))
        print(f"{Path(graph).stem}: {count} eligible sources", flush=True)
        rows.append([graph, grammar, expected, str(pool)])
    pool_config = args.directory / "pools.csv"
    if not pool_config.exists():
        write_csv(pool_config, rows)
    elif read_config(pool_config, 4) != rows:
        raise ValueError(f"{pool_config}: pool config differs from the prepared graphs")


def benchmark(stage, config, algorithm, raw, rounds, *, size=None, chunks=None, seed=None, paired=False):
    argv = [BENCH, "-c", config, "-a", algorithm, "-r", rounds, "--hot", "--results-file", raw]
    if algorithm != "CFL_CFPQ_RSM":
        argv += ["-efbl"]
    if size is not None:
        argv += ["--experiment", "--source-sizes", size, "--chunks", chunks, "--seed", seed]
    if paired:
        argv += ["--compare-rsm"]
    if paired or algorithm == "CFL_CFPQ_RSM":
        argv += ["--rsm-lib", RSM_LIB]
    command(stage, argv)


def timing_blocks(path, configs, algorithms, size, chunks, rounds):
    """Validate the exact order and size of the ordinary benchmark CSV."""
    with path.open(newline="") as stream:
        rows = csv.DictReader(stream)
        for graph, grammar, *_ in configs:
            for chunk in range(chunks):
                medians = {}
                results = set()
                for algorithm in algorithms:
                    times = []
                    signatures = set()
                    for _ in range(rounds):
                        row = next(rows, None)
                        key = (graph, grammar, size, chunk, algorithm)
                        if (
                            row is None
                            or (row["graph"], row["cnf"], int(row["source_size"]), int(row["chunk"]), row["algo_name"])
                            != key
                        ):
                            raise ValueError(f"{path}: missing or out-of-order timing row for {key}")
                        elapsed = float(row["time_s"])
                        if not math.isfinite(elapsed) or elapsed < 0:
                            raise ValueError(f"{path}: invalid time for {key}")
                        times.append(elapsed)
                        signatures.add((int(row["result"]), int(row["result_hash"])))
                    if len(signatures) != 1:
                        raise ValueError(f"{path}: inconsistent result for {key}")
                    results.add(signatures.pop()[0])
                    medians[algorithm] = median(times)
                if len(results) != 1:
                    raise ValueError(f"{path}: CFL and RSM returned different counts for {graph}, chunk {chunk}")
                yield graph, chunk + 1, medians
        if next(rows, None) is not None:
            raise ValueError(f"{path}: extra rows after the requested benchmark runs")


def find_interval(pool_size, measure, cutoffs):
    for size in range(10, pool_size + 1, 10):
        if measure("pilot", size, PILOT_QUERIES) >= PILOT_WINS:
            break
    else:
        return "no_pilot_upper", None, None

    upper_test = cutoffs["upper"]
    for upper in range(size, pool_size + 1, 3):
        if measure("upper", upper, upper_test["queries"]) >= upper_test["min_wins"]:
            break
    else:
        return "no_confirmed_upper", None, None

    size = max(1, upper - 10)
    lower_test = cutoffs["lower"]
    while True:
        wins = measure("lower", size, lower_test["queries"])
        if lower_test["queries"] - wins >= lower_test["min_losses"]:
            return "confirmed", size + 1, upper
        if size == 1:
            return "upper_only", None, upper
        size = max(1, size - 5)


def paired(args):
    config = args.directory / "pools.csv"
    pools = read_config(config, 4)
    cutoffs = CONFIRMATION_CUTOFFS
    if args.seed == args.confirm_seed:
        raise ValueError("pilot and confirmation must use different seeds")
    stage = stage_dir(args.directory, "paired")
    write_json(
        stage / "settings.json",
        {
            "pilot_seed": args.seed,
            "confirmation_seed": args.confirm_seed,
            "rounds": ROUNDS,
            "pilot_queries": PILOT_QUERIES,
            "pilot_min_wins": PILOT_WINS,
            "cutoffs": cutoffs,
            "benchmark": file_record(BENCH),
            "candidate": file_record(RSM_LIB),
            "config": file_record(config),
            "pools": [file_record(row[3]) for row in pools],
        },
    )
    algorithms = ("CFL_multsrc_adv", "CFL_CFPQ_RSM")
    with (
        (stage / "points.csv").open("x", newline="") as points,
        (stage / "intervals.csv").open("x", newline="") as summary,
    ):
        points_writer = csv.writer(points)
        summary_writer = csv.writer(summary)
        points_writer.writerow(("graph", "phase", "source_size", "queries", "rsm_wins", "status", "raw_csv"))
        summary_writer.writerow(("graph", "status", "lower", "upper"))

        def measure(row, phase, size, queries):
            name = Path(row[0]).stem
            graph_dir = stage / name
            graph_config = graph_dir / "graph.csv"
            raw = graph_dir / f"{phase}-{size}.csv"
            seed = args.seed if phase == "pilot" else args.confirm_seed
            benchmark(
                stage, graph_config, algorithms[0], raw, ROUNDS, size=size, chunks=queries, seed=seed, paired=True
            )
            wins = sum(
                times[algorithms[1]] <= times[algorithms[0]]
                for _, _, times in timing_blocks(raw, [row], algorithms, size, queries, ROUNDS)
            )
            if phase == "lower":
                passed = queries - wins >= cutoffs[phase]["min_losses"]
            else:
                threshold = PILOT_WINS if phase == "pilot" else cutoffs[phase]["min_wins"]
                passed = wins >= threshold
            status = "passed" if passed else "unresolved"
            points_writer.writerow((name, phase, size, queries, wins, status, raw.relative_to(stage)))
            points.flush()
            print(f"{name} {phase} s={size}: {wins}/{queries} RSM wins ({status})", flush=True)
            return wins

        for row in pools:
            name = Path(row[0]).stem
            graph_dir = stage / name
            graph_dir.mkdir()
            graph_config = graph_dir / "graph.csv"
            write_csv(graph_config, [row])

            status, lower, upper = find_interval(len(read_pool(row[3])), partial(measure, row), cutoffs)
            summary_writer.writerow((name, status, lower, upper))
            summary.flush()
    print(f"Intervals: {stage / 'intervals.csv'}")


def cfl(args):
    config = args.directory / "graphs.csv"
    graphs = read_config(config, 3)
    stage = stage_dir(args.directory, "cfl-adv")
    raw = stage / "timings.csv"
    benchmark(stage, config, "CFL_adv", raw, CFL_ROUNDS)
    for _ in timing_blocks(raw, graphs, ("CFL_adv",), 0, CFL_ROUNDS, 1):
        pass
    print(f"CFL_adv raw timings: {raw}")


def rsm(args):
    config = args.directory / "pools.csv"
    pools = read_config(config, 4)
    for graph, _, _, pool in pools:
        if len(read_pool(pool)) < RSM_SIZE:
            raise ValueError(f"{graph}: fewer than {RSM_SIZE} eligible sources")
    stage = stage_dir(args.directory, "rsm-s100")
    write_json(
        stage / "settings.json",
        {
            "seed": args.seed,
            "source_size": RSM_SIZE,
            "queries_per_graph": RSM_QUERIES,
            "rounds": ROUNDS,
            "benchmark": file_record(BENCH),
            "candidate": file_record(RSM_LIB),
            "config": file_record(config),
            "pools": [file_record(row[3]) for row in pools],
        },
    )
    raw = stage / "timings.csv"
    algorithm = "CFL_CFPQ_RSM"
    benchmark(stage, config, algorithm, raw, ROUNDS, size=RSM_SIZE, chunks=RSM_QUERIES, seed=args.seed)
    summary = stage / "query-medians.csv"
    with summary.open("x", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("graph", "source_size", "query", "median_time_s"))
        for graph, query, times in timing_blocks(raw, pools, (algorithm,), RSM_SIZE, RSM_QUERIES, ROUNDS):
            writer.writerow((Path(graph).stem, RSM_SIZE, query, times[algorithm]))
    print(f"RSM raw timings: {raw}\nQuery medians: {summary}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="phase", required=True)
    for action in (prepare, paired, cfl, rsm):
        sub = commands.add_parser(action.__name__)
        sub.set_defaults(action=action)
        sub.add_argument("directory", type=Path, help="campaign directory")
    commands.choices["prepare"].add_argument("--config", type=Path, default=CONFIG)
    commands.choices["prepare"].add_argument("--reuse-candidates", type=Path)
    commands.choices["paired"].add_argument("--seed", type=master_seed, default=2040)
    commands.choices["paired"].add_argument("--confirm-seed", type=master_seed, default=2041)
    commands.choices["rsm"].add_argument("--seed", type=master_seed, default=2042)
    args = parser.parse_args()
    os.chdir(ROOT)
    args.directory = args.directory.resolve()
    if args.phase == "prepare" and args.reuse_candidates is not None:
        args.reuse_candidates = args.reuse_candidates.resolve()
    try:
        args.action(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{args.phase}: {error}. See the printed run directory's run.log for benchmark output.\n")


if __name__ == "__main__":
    main()
