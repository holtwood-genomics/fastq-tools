#!/usr/bin/env python3
"""Run the production benchmark suite and summarize repeated measurements.

Each executable is run with a fixed five-repetition contract. The raw Google
Benchmark JSON is retained next to a compact median/CV summary so a baseline
can be audited without rerunning the workload.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from statistics import median, pstdev
from typing import Any, Dict, List, Optional


# 探索性矩阵按需运行，不能因为目录里存在可执行文件就混入固定基线。
EXPLORATORY_BENCHMARKS = {"benchmark_concurrency"}


def benchmark_executables(build_dir: Path, include_exploratory: bool) -> List[Path]:
    directory = build_dir / "tools" / "benchmark"
    return sorted(
        path
        for path in directory.glob("benchmark_*")
        if path.is_file() and (include_exploratory or path.stem not in EXPLORATORY_BENCHMARKS)
    )


def run_executable(
    executable: Path, output_path: Path, repetitions: int, benchmark_filter: Optional[str]
) -> Optional[Dict[str, Any]]:
    command = [
        str(executable),
        "--benchmark_format=json",
        f"--benchmark_out={output_path}",
        f"--benchmark_repetitions={repetitions}",
        "--benchmark_min_time=0.001s",
        "--benchmark_report_aggregates_only=false",
    ]
    if benchmark_filter:
        command.append(f"--benchmark_filter={benchmark_filter}")
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        output = f"{completed.stdout}\n{completed.stderr}"
        if "Failed to match any benchmarks" in output:
            output_path.unlink(missing_ok=True)
            print(f"Skipping {executable.name}: filter matched no benchmarks")
            return None
        print(output, file=sys.stderr, end="")
        raise subprocess.CalledProcessError(completed.returncode, command)
    if not output_path.exists() or output_path.stat().st_size == 0:
        output_path.unlink(missing_ok=True)
        return None
    return json.loads(output_path.read_text(encoding="utf-8"))


def sample_name(name: str) -> str:
    return re.sub(r"_(?:mean|median|stddev)$", "", name)


def is_iteration(entry: Dict[str, Any]) -> bool:
    run_type = entry.get("run_type")
    if run_type:
        return run_type == "iteration"
    name = str(entry.get("name", ""))
    return not name.endswith(("_mean", "_median", "_stddev"))


def counter(entry: Dict[str, Any], key: str) -> float:
    nested = entry.get("counters", {})
    return float(entry.get(key, nested.get(key, 0.0)) or 0.0)


_TIME_UNIT_SECONDS = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0}


def time_seconds(entry: Dict[str, Any], key: str) -> float:
    value = float(entry.get(key, 0.0) or 0.0)
    unit = str(entry.get("time_unit", "ns"))
    return value * _TIME_UNIT_SECONDS.get(unit, 1e-9)


def wall_seconds(entry: Dict[str, Any]) -> float:
    """优先使用 harness 自己记录的 steady_clock 时间。

    旧版 benchmark 没有 wall_seconds，回退到 cpu_time，而不是被 WSL2 放大的
    real_time。这样旧 JSON 也能生成可解释的 CPU 口径摘要。
    """
    wall = counter(entry, "wall_seconds")
    return wall if wall > 0.0 else time_seconds(entry, "cpu_time")


def read_count(entry: Dict[str, Any]) -> float:
    explicit = counter(entry, "read_count")
    if explicit > 0.0:
        return explicit
    rate = counter(entry, "reads_per_s")
    wall = wall_seconds(entry)
    return rate * wall if rate > 0.0 and wall > 0.0 else 0.0


def input_bytes(entry: Dict[str, Any]) -> float:
    explicit = counter(entry, "input_bytes")
    if explicit > 0.0:
        return explicit
    rate = counter(entry, "mib_per_s")
    wall = wall_seconds(entry)
    return rate * 1024.0 * 1024.0 * wall if rate > 0.0 and wall > 0.0 else 0.0


def summarize(data: Dict[str, Any], source: str, repetitions: int) -> Dict[str, Any]:
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for entry in data.get("benchmarks", []):
        if is_iteration(entry):
            grouped.setdefault(sample_name(str(entry.get("name", ""))), []).append(entry)

    results: List[Dict[str, Any]] = []
    violations: List[str] = []
    for name, entries in sorted(grouped.items()):
        wall = [wall_seconds(entry) for entry in entries]
        cpu = [time_seconds(entry, "cpu_time") for entry in entries]
        reads = [read_count(entry) for entry in entries]
        bytes_read = [input_bytes(entry) for entry in entries]
        memory = [counter(entry, "peak_memory_bytes") for entry in entries]

        read_rates = [r / w for r, w in zip(reads, wall) if r > 0.0 and w > 0.0]
        mib_rates = [
            b / (1024.0 * 1024.0) / w
            for b, w in zip(bytes_read, wall)
            if b > 0.0 and w > 0.0
        ]

        mean_wall = sum(wall) / len(wall) if wall else 0.0
        mean_cpu = sum(cpu) / len(cpu) if cpu else 0.0
        wall_cv = (pstdev(wall) / mean_wall * 100.0) if len(wall) > 1 and mean_wall else 0.0
        cpu_cv = (pstdev(cpu) / mean_cpu * 100.0) if len(cpu) > 1 and mean_cpu else 0.0
        has_wall_counter = any(counter(entry, "wall_seconds") > 0.0 for entry in entries)
        lower_name = name.lower()
        target_cv = 10.0 if any(part in lower_name for part in ("reader", "writer", "filter")) else 5.0
        effective_cv = wall_cv if has_wall_counter else cpu_cv
        status = "pass" if effective_cv <= target_cv else "high-cv"
        if status != "pass":
            violations.append(name)

        results.append(
            {
                "name": name,
                "repetitions": len(entries),
                # 兼容旧报告字段：median_time 现在表示 wall time（ms）；没有 wall 时是 CPU time。
                "median_time": median(wall) * 1000.0 if wall else 0.0,
                "median_cpu_time": median(cpu) * 1000.0 if cpu else 0.0,
                "median_wall_seconds": median(wall) if wall else 0.0,
                "median_cpu_seconds": median(cpu) if cpu else 0.0,
                "median_reads_per_s": median(read_rates) if read_rates else 0.0,
                "median_mib_per_s": median(mib_rates) if mib_rates else 0.0,
                "peak_memory_bytes": max(memory) if memory else 0.0,
                "time_source": "steady_clock" if has_wall_counter else "cpu_time",
                "cv_percent": wall_cv if has_wall_counter else cpu_cv,
                "cpu_cv_percent": cpu_cv,
                "target_cv_percent": target_cv,
                "cv_status": status,
            }
        )

    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "source": source,
        "repetitions_requested": repetitions,
        "benchmarks": results,
        "cv_violations": violations,
    }


def write_summary(summary: Dict[str, Any], path: Path) -> None:
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("Benchmark summary:")
    print(
        "name | wall ms | cpu ms | reads/s | MiB/s | peak memory | "
        "wall CV | CPU CV | target | status"
    )
    for result in summary["benchmarks"]:
        print(
            f"{result['name']} | {result['median_time']:.3f} | "
            f"{result['median_cpu_time']:.3f} | "
            f"{result['median_reads_per_s']:.1f} | {result['median_mib_per_s']:.1f} | "
            f"{result['peak_memory_bytes']:.0f} | {result['cv_percent']:.2f}% | "
            f"{result['cpu_cv_percent']:.2f}% | {result['target_cv_percent']:.0f}% | "
            f"{result['cv_status']}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--filter", help="Google Benchmark filter passed to each executable")
    parser.add_argument(
        "--include-exploratory",
        action="store_true",
        help="Include benchmark_concurrency in the run (excluded from the fixed baseline by default)",
    )
    parser.add_argument("--enforce-cv", action="store_true")
    args = parser.parse_args()

    if args.repetitions < 2:
        parser.error("--repetitions must be at least 2")

    executables = benchmark_executables(args.build_dir, args.include_exploratory)
    if not executables:
        print(f"No benchmark executables found in {args.build_dir}", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    summaries = []
    for executable in executables:
        raw_path = args.output_dir / f"{executable.name}.json"
        data = run_executable(executable, raw_path, args.repetitions, args.filter)
        if data is None:
            continue
        summary = summarize(data, executable.name, args.repetitions)
        write_summary(summary, args.output_dir / f"{executable.name}.summary.json")
        summaries.append(summary)

    violations = [name for summary in summaries for name in summary["cv_violations"]]
    if args.enforce_cv and violations:
        print(f"CV target exceeded: {', '.join(violations)}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
