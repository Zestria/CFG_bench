"""Prepare and run the coursework timing experiment.

The script:
1. computes candidate start vertices with pairs_extractor --start-only;
2. writes a config whose fourth column points to those candidate files;
3. runs CFL_adv once per graph and the multiple-source algorithms on 30 chunks
   for each source size;
4. aggregates the raw results into mean/median tables.

Results go to results/experiment_<timestamp>/ by default.
"""

import argparse
import datetime
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "configs" / "experiment.csv"
DEFAULT_ALGORITHMS = ["CFL_adv", "CFL_CFPQ_RSM", "CFL_multsrc_adv"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="graph/grammar config to run")
    parser.add_argument("--out-dir", type=Path, help="output directory (default: results/experiment_<timestamp>)")
    parser.add_argument("--reuse-candidates", type=Path, help="use candidate files from this directory")
    parser.add_argument("--algorithms", default=",".join(DEFAULT_ALGORITHMS), help="comma-separated algorithms to run")
    parser.add_argument("--source-sizes", default="1,10,100", help="comma-separated source set sizes")
    parser.add_argument("--chunks", type=int, default=30, help="number of chunks per source size (default: 30)")
    parser.add_argument("--rounds", type=int, default=3, help="measured runs per chunk (default: 3)")
    parser.add_argument("--adv-rounds", type=int, default=10, help="measured CFL_adv runs (default: 10)")
    parser.add_argument("--seed", type=int, default=2026, help="source shuffle seed (default: 2026)")
    parser.add_argument("--force", action="store_true", help="overwrite existing raw result files")
    parser.add_argument("--no-build", action="store_true", help="do not run make before the experiment")
    parser.add_argument("--no-aggregate", action="store_true", help="do not run aggregate_experiment.py")
    return parser.parse_args()


def read_config(path: Path) -> list[tuple[str, str]]:
    rows = []
    with open(path) as file:
        for line in file:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split(",")]
            if len(fields) < 2:
                raise ValueError(f"Invalid config row: {line}")
            rows.append((fields[0], fields[1]))
    if not rows:
        raise ValueError(f"Config is empty: {path}")
    return rows


def candidate_name(graph: str, grammar: str) -> str:
    return f"{Path(grammar).stem}_{Path(graph).stem}_start.result"


def write_config_with_sources(path: Path, rows: list[tuple[str, str]], candidates: Path) -> None:
    with open(path, "w") as file:
        file.writelines(
            f"{graph},{grammar},-1,{candidates / candidate_name(graph, grammar)}\n" for graph, grammar in rows
        )


def run_command(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def git_metadata() -> list[str]:
    lines = []
    for name, command in [
        ("branch", ["git", "rev-parse", "--abbrev-ref", "HEAD"]),
        ("commit", ["git", "rev-parse", "--short", "HEAD"]),
    ]:
        try:
            value = subprocess.run(command, cwd=ROOT, check=True, text=True, capture_output=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            value = "unknown"
        lines.append(f"{name}: {value}")
    return lines


def main() -> int:
    args = parse_args()
    algorithms = [name.strip() for name in args.algorithms.split(",") if name.strip()]
    if not algorithms:
        raise ValueError("At least one algorithm is required")
    for name in algorithms:
        if name not in DEFAULT_ALGORITHMS:
            raise ValueError(f"Unknown algorithm: {name}")
    if args.chunks <= 0 or args.rounds <= 0 or args.adv_rounds <= 0:
        raise ValueError("--chunks, --rounds and --adv-rounds must be positive")
    if args.seed < 0 or args.seed > 2**32 - 1:
        raise ValueError("--seed must be an unsigned 32-bit integer")
    try:
        source_sizes = [int(value.strip()) for value in args.source_sizes.split(",") if value.strip()]
    except ValueError as error:
        raise ValueError("--source-sizes must be a comma-separated list of integers") from error
    if not source_sizes or any(size <= 0 for size in source_sizes):
        raise ValueError("--source-sizes must contain positive integers")
    if len(set(source_sizes)) != len(source_sizes):
        raise ValueError("--source-sizes must not contain duplicates")
    args.source_sizes = ",".join(str(size) for size in source_sizes)

    config_path = args.config if args.config.is_absolute() else ROOT / args.config
    out_dir = args.out_dir
    if out_dir is None:
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d_%H-%M-%S")
        out_dir = ROOT / "results" / f"experiment_{stamp}"
    elif not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    candidates_dir = args.reuse_candidates
    if candidates_dir is not None and not candidates_dir.is_absolute():
        candidates_dir = ROOT / candidates_dir
    if candidates_dir is None:
        candidates_dir = out_dir / "candidates"
    candidates_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = out_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    config_with_sources = out_dir / "config_with_sources.csv"

    rows = read_config(config_path)
    if not args.no_build:
        run_command(["make"])

    if args.reuse_candidates is None:
        run_command(
            [
                str(ROOT / "build" / "pairs_extractor"),
                "-c",
                str(config_path),
                "-o",
                str(candidates_dir),
                "--start-only",
            ]
        )
    else:
        print(f"Reusing candidate files from {candidates_dir}")

    missing = []
    for graph, grammar in rows:
        path = candidates_dir / candidate_name(graph, grammar)
        if not path.is_file() or path.stat().st_size == 0:
            missing.append(str(path))
    if missing:
        raise FileNotFoundError("Missing or empty candidate files:\n  " + "\n  ".join(missing))

    write_config_with_sources(config_with_sources, rows, candidates_dir)

    metadata = [
        f"date: {datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds')}",
        *git_metadata(),
        f"config: {config_path}",
        f"candidates: {candidates_dir}",
        f"algorithms: {', '.join(algorithms)}",
        f"source_sizes: {args.source_sizes}",
        f"chunks: {args.chunks}",
        f"rounds_per_chunk: {args.rounds}",
        f"cfl_adv_rounds: {args.adv_rounds}",
        f"seed: {args.seed}",
    ]
    (out_dir / "metadata.txt").write_text("\n".join(metadata) + "\n")

    raw_paths = []
    for name in algorithms:
        raw_path = raw_dir / f"{name}.csv"
        if raw_path.exists():
            if not args.force:
                print(f"Skipping {name}: {raw_path} already exists (use --force to rerun)")
                raw_paths.append(raw_path)
                continue
            raw_path.unlink()

        command = [
            str(ROOT / "build" / "cfg_bench"),
            "-c",
            str(config_with_sources),
            "-a",
            name,
            "--results-file",
            str(raw_path),
        ]
        if name == "CFL_adv":
            command += ["-efbl", "-r", str(args.adv_rounds), "--hot"]
        else:
            command += [
                "--experiment",
                "--source-sizes",
                args.source_sizes,
                "--chunks",
                str(args.chunks),
                "--seed",
                str(args.seed),
                "-r",
                str(args.rounds),
                "--hot",
            ]
            if name == "CFL_multsrc_adv":
                command += ["-efbl"]

        run_command(command)
        raw_paths.append(raw_path)

    if not args.no_aggregate:
        run_command(
            [
                sys.executable,
                str(ROOT / "tools" / "aggregate_experiment.py"),
                *[str(path) for path in raw_paths],
                "--output",
                str(out_dir / "summary.csv"),
                "--per-chunk",
                str(out_dir / "per_chunk.csv"),
                "--chunks",
                str(args.chunks),
                "--repeats",
                str(args.rounds),
                "--all-pairs-runs",
                str(args.adv_rounds),
            ]
        )

    print(f"\nExperiment output: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
