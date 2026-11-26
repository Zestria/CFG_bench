"""Prepare eligible sources and run the paired Java Points-To search."""

import argparse
import csv
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
from functools import partial
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/optimized"
CONFIG = ROOT / "configs/java_experiment.csv"
CUTOFFS = {"pilot": (150, 135), "upper": (298, 277), "lower": (474, 59)}
TIME_COLUMNS = tuple(tuple(f"{algorithm}_ns_{i}" for i in range(1, 4)) for algorithm in ("cfl", "rsm"))


def read_config(path, columns):
    with path.open(newline="") as stream:
        rows = [row for row in csv.reader(stream) if row]
    if not rows or any(
        len(row) != columns or any(not field or any(c in field for c in ',\r\n"') for field in row) for row in rows
    ):
        raise ValueError(f"{path}: expected a headerless CFG_bench config with {columns} columns")
    return rows


def write_csv(path, rows):
    with path.open("x", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)


def command(log_path, argv):
    args = list(map(str, argv))
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    env.pop("LD_PRELOAD", None)
    env.setdefault("GRAPHBLAS_CACHE_PATH", str(ROOT / "build/dependencies/graphblas/jit-cache"))
    with log_path.open("a") as log:
        print(shlex.join(args), file=log, flush=True)
        subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)


def pool_size(path):
    size = 0
    previous = -1
    with path.open() as stream:
        for line in stream:
            text = line.strip()
            if not text:
                continue
            if not (text.isascii() and text.isdecimal()):
                raise ValueError(f"{path}: expected one nonnegative vertex ID per line")
            node = int(text)
            if node <= previous:
                raise ValueError(f"{path}: vertex IDs must be strictly increasing")
            previous = node
            size += 1
    return size


def prepare(args):
    rows = read_config(args.config, 3)
    names = [f"{Path(cnf).stem}_{Path(graph).stem}_start.result" for graph, cnf, _ in rows]
    if len(set(names)) != len(names):
        raise ValueError("Graph and grammar filenames produce duplicate candidate pool names")

    output = args.directory / "pools.csv"
    if output.exists():
        raise ValueError(f"{output} already exists. Use it for paired runs or choose another campaign directory")
    args.directory.mkdir(parents=True, exist_ok=True)
    if args.reuse_candidates is None:
        pool_dir = Path(tempfile.mkdtemp(prefix="start_nodes.", dir=args.directory))
        command(
            args.directory / "prepare.log",
            [BUILD / "pairs_extractor", "-c", args.config, "-o", pool_dir, "--start-only"],
        )
    else:
        pool_dir = args.reuse_candidates
    pools = []
    for row, name in zip(rows, names, strict=True):
        path = pool_dir / name
        print(f"{row[0]}: {pool_size(path)} eligible sources", flush=True)
        pools.append([*row, str(path)])
    write_csv(output, pools)
    print(output)


def point_seed(seed, row, phase, size):
    data = json.dumps([seed, *row[:2], phase, size]).encode()
    return int.from_bytes(hashlib.blake2b(data, digest_size=8).digest(), "big")


def count_wins(path, size, queries):
    wins = 0
    count = 0
    with path.open(newline="") as stream:
        for count, row in enumerate(csv.DictReader(stream), start=1):
            try:
                if int(row["query"]) != count or len(row["sources"].split()) != size:
                    raise ValueError("wrong query ID or source count")
                cfl, rsm = ([int(row[key]) for key in columns] for columns in TIME_COLUMNS)
                if min(cfl + rsm) < 0:
                    raise ValueError("negative measurement")
            except (KeyError, TypeError, ValueError, AttributeError) as error:
                raise ValueError(f"{path}: invalid row for query {count}: {error}") from error
            wins += median(rsm) <= median(cfl)
    if count != queries:
        raise ValueError(f"{path}: expected {queries} paired queries, got {count}")
    return wins


def find_interval(size_of_pool, measure):
    for size in range(10, size_of_pool + 1, 10):
        if measure("pilot", size):
            break
    else:
        return "no_pilot_upper", None, None

    for upper in range(size, size_of_pool + 1, 3):
        if measure("upper", upper):
            break
    else:
        return "no_confirmed_upper", None, None

    size = max(1, upper - 10)
    while True:
        if measure("lower", size):
            return "confirmed", size + 1, upper
        if size == 1:
            return "upper_only", None, upper
        size = max(1, size - 3)


def paired(args):
    csv.field_size_limit(sys.maxsize)
    config = args.directory / "pools.csv"
    pools = [(row, pool_size(ROOT / row[3])) for row in read_config(config, 4)]
    stage = Path(tempfile.mkdtemp(prefix="paired.", dir=args.directory))
    print(f"Run directory: {stage}", flush=True)
    cutoffs = {
        phase: {"queries": queries, "min_losses" if phase == "lower" else "min_wins": cutoff}
        for phase, (queries, cutoff) in CUTOFFS.items()
    }
    settings = {"seed": args.seed, "pools": str(config), "cutoffs": cutoffs}
    (stage / "settings.json").write_text(json.dumps(settings, indent=2) + "\n")

    with (
        (stage / "points.csv").open("x", newline="") as points,
        (stage / "intervals.csv").open("x", newline="") as intervals,
    ):
        points_writer = csv.writer(points)
        intervals_writer = csv.writer(intervals)
        points_writer.writerow(
            ("graph", "cnf", "phase", "source_size", "queries", "seed", "rsm_wins", "status", "raw_csv")
        )
        intervals_writer.writerow(("graph", "cnf", "status", "lower", "upper"))
        points.flush()
        intervals.flush()

        def measure(row, graph_dir, phase, size):
            queries, cutoff = CUTOFFS[phase]
            seed = point_seed(args.seed, row, phase, size)
            raw = graph_dir / f"{phase}-{size}.csv"
            command(
                stage / "run.log",
                [
                    BUILD / "cfg_bench",
                    "-c",
                    graph_dir / "graph.csv",
                    "--experiment",
                    "--source-size",
                    size,
                    "--chunks",
                    queries,
                    "--seed",
                    seed,
                    "--results-file",
                    raw,
                ],
            )
            wins = count_wins(raw, size, queries)
            passed = (queries - wins if phase == "lower" else wins) >= cutoff
            status = "passed" if passed else "unresolved"
            points_writer.writerow((*row[:2], phase, size, queries, seed, wins, status, raw.relative_to(stage)))
            points.flush()
            print(f"{row[0]} {phase} s={size}: {wins}/{queries} RSM wins ({status})", flush=True)
            return passed

        for i, (row, size) in enumerate(pools, start=1):
            graph_dir = stage / f"{i:02d}-{Path(row[0]).stem}"
            graph_dir.mkdir()
            write_csv(graph_dir / "graph.csv", [row])
            status, lower, upper = find_interval(size, partial(measure, row, graph_dir))
            intervals_writer.writerow((*row[:2], status, lower, upper))
            intervals.flush()
    print(f"Intervals: {stage / 'intervals.csv'}")


def master_seed(text):
    value = int(text)
    if not 0 <= value < 2**64:
        raise argparse.ArgumentTypeError("seed must be an unsigned 64-bit integer")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="phase", required=True)
    for action in (prepare, paired):
        subparser = commands.add_parser(action.__name__)
        subparser.set_defaults(action=action)
        subparser.add_argument("directory", type=lambda value: Path(value).resolve(), help="campaign directory")
    commands.choices["prepare"].add_argument("--config", type=lambda value: Path(value).resolve(), default=CONFIG)
    commands.choices["prepare"].add_argument("--reuse-candidates", type=lambda value: Path(value).resolve())
    commands.choices["paired"].add_argument("--seed", type=master_seed, default=2026)
    args = parser.parse_args()
    try:
        args.action(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{args.phase}: {error}\nLogs are in the campaign directory and its run subdirectories.\n")


if __name__ == "__main__":
    main()
