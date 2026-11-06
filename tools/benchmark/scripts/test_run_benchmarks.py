#!/usr/bin/env python3
"""run_benchmarks.py 的单元测试：不依赖 Google Benchmark 可执行文件。"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from statistics import median
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_benchmarks.py")
SPEC = importlib.util.spec_from_file_location("run_benchmarks", MODULE_PATH)
run_benchmarks = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(run_benchmarks)


class BenchmarkExecutablesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bench_dir = Path(self.tmp.name) / "tools" / "benchmark"
        self.bench_dir.mkdir(parents=True)
        for name in ("benchmark_fastq_io", "benchmark_concurrency", "benchmark_pipeline"):
            (self.bench_dir / name).write_text("", encoding="utf-8")
        (self.bench_dir / "benchmark_not_a_file").mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_excludes_exploratory_by_default(self):
        names = [p.name for p in run_benchmarks.benchmark_executables(Path(self.tmp.name), False)]
        self.assertIn("benchmark_fastq_io", names)
        self.assertIn("benchmark_pipeline", names)
        self.assertNotIn("benchmark_concurrency", names)
        self.assertNotIn("benchmark_not_a_file", names)

    def test_includes_exploratory_on_request(self):
        names = [p.name for p in run_benchmarks.benchmark_executables(Path(self.tmp.name), True)]
        self.assertIn("benchmark_concurrency", names)


class SummarizeTest(unittest.TestCase):
    def test_uses_wall_seconds_when_present(self):
        data = {
            "benchmarks": [
                {
                    "name": "benchmarkReaderGzip/iterations:1",
                    "run_type": "iteration",
                    "wall_seconds": 0.5,
                    "read_count": 1000.0,
                    "input_bytes": 1048576.0,
                    "cpu_time": 0.25,
                    "time_unit": "s",
                    "counters": {"peak_memory_bytes": 123.0},
                },
                {
                    "name": "benchmarkReaderGzip/iterations:1",
                    "run_type": "iteration",
                    "wall_seconds": 0.7,
                    "read_count": 1000.0,
                    "input_bytes": 1048576.0,
                    "cpu_time": 0.35,
                    "time_unit": "s",
                    "counters": {"peak_memory_bytes": 130.0},
                },
                {
                    "name": "benchmarkReaderGzip/iterations:1_mean",
                    "run_type": "aggregate",
                    "wall_seconds": 0.6,
                    "cpu_time": 0.3,
                    "time_unit": "s",
                },
            ]
        }
        summary = run_benchmarks.summarize(data, "benchmark_fastq_io", 2)
        self.assertEqual(len(summary["benchmarks"]), 1)
        result = summary["benchmarks"][0]
        self.assertEqual(result["time_source"], "steady_clock")
        self.assertAlmostEqual(result["median_wall_seconds"], 0.6)
        self.assertAlmostEqual(result["median_time"], 600.0)
        # runner 取每次迭代速率的 median，而不是 median(wall) 反算的速率。
        self.assertAlmostEqual(result["median_reads_per_s"], median([1000.0 / 0.5, 1000.0 / 0.7]))
        self.assertAlmostEqual(result["median_mib_per_s"], median([1.0 / 0.5, 1.0 / 0.7]))
        self.assertEqual(result["peak_memory_bytes"], 130.0)

    def test_falls_back_to_cpu_time(self):
        data = {
            "benchmarks": [
                {
                    "name": "benchmarkReader/iterations:1",
                    "run_type": "iteration",
                    "cpu_time": 1000.0,
                    "time_unit": "ms",
                    "counters": {
                        "reads_per_s": 2000.0,
                        "mib_per_s": 10.0,
                        "peak_memory_bytes": 1.0,
                    },
                },
                {
                    "name": "benchmarkReader/iterations:1",
                    "run_type": "iteration",
                    "cpu_time": 1200.0,
                    "time_unit": "ms",
                    "counters": {
                        "reads_per_s": 2000.0,
                        "mib_per_s": 10.0,
                        "peak_memory_bytes": 1.0,
                    },
                },
            ]
        }
        summary = run_benchmarks.summarize(data, "benchmark_fastq_io", 2)
        result = summary["benchmarks"][0]
        self.assertEqual(result["time_source"], "cpu_time")
        self.assertAlmostEqual(result["median_time"], 1100.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
