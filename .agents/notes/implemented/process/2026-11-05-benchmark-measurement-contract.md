# Agent Note: Benchmark 测量契约——steady_clock + CPU 双时钟

Status: implemented

## Problem

v4 生产基准的摘要层按 benchmark 名称选择时间字段：名字含 `stat` 用 `cpu_time`，其余用 `real_time`。在 WSL2 上 `real_time` 会被放大到不可用（环境说明已记录约 `1e7 ms`），导致同一份报告里 `median_time`、`reads_per_s` 与 CV 不是同一个时钟口径。例如 `gzip_6_single` 的 `reads/s` 反推约 139 s，对应 `cpu_time`，但摘要的 `median_time` 取 `real_time`，两者相差两个数量级。writer benchmark 还漏掉了 `FastqWriter::finish()`，没有测 gzip close、`fsync` 和原子 rename；filter 基准固定单线程，gzip 输入也没有覆盖。

结论：旧数据不能用于判断优化收益，先把“尺子”修好。

## Decision

1. 每个 benchmark 在 harness 内用 `std::chrono::steady_clock` 记录整个计时循环的 wall time，写入每次迭代的 `wall_seconds`；同时保留 Google Benchmark 的 `cpu_time`。
2. 写入 `read_count`、`input_bytes`，由 `run_benchmarks.py` 自行计算 reads/s 与 MiB/s，不再依赖 benchmark 库内部的 rate counter。
3. writer benchmark 在计时循环内调用 `finish()`，测量真实生产发布路径。
4. `benchmark_fastq_io` 增加 gzip 输入场景；`benchmark_concurrency` 增加 gzip 输出变体，并新增 `benchmarks_concurrency` 目标。`run_benchmarks.py` 默认按名字排除 `benchmark_concurrency`，只有显式 `--include-exploratory` 才纳入，避免“构建过探索性目标”污染固定基线。
5. 旧报告没有 `wall_seconds` 时回退到 `cpu_time`，并在 summary 标记 `time_source=cpu_time`；不假装旧数据与新数据同口径。

## Alternatives considered

- **继续用 Google Benchmark `real_time`，只修 WSL2 环境** — 被否：测量契约不应依赖运行环境恰好正确；在 CI/容器/WSL 上仍会静默产出错误摘要。
- **只用 `cpu_time`** — 被否：I/O 和多线程 wall 吞吐会被低估或无法解释；CPU time 也不能直接代表用户感知延迟。
- **外部 `hyperfine`/`perf stat` 替代进程内 harness** — 被否：会丢失 Google Benchmark 的固定数据集、重复次数和 counters；外部工具适合做交叉验证，不作为唯一入口。
- **writer benchmark 保持不调用 `finish()`，另建生产基准** — 被否：默认套件会继续低估 writer 成本；直接修默认场景更诚实。

## Consequences

- **收益**：同一提交的 wall/CPU/CV 可解释；gzip 读写和线程扩展有可复核入口；旧报告仍可生成 CPU 口径摘要。
- **代价与已知上限**：wall time 仍受机器负载影响，绝对数值只在同机同配置下比较；`steady_clock` 不能替代 `perf` 热点归因；小文件/冷缓存仍会污染 I/O 场景，需要预热与固定机器。
- **旧数据边界**：v4 `median_time`/CV 不再作为优化验收依据；新报告与旧报告只能做“方向性”对照，不能做绝对倍数比较。

## Verification

在 Ryzen 7 8845HS / 12 线程 / Clang 18 Release / 1M×150 bp 上：

- plain reader：`wall_seconds=0.0966`，`observed_reads=1,000,000`，约 10.3M reads/s（页缓存命中；旧报告约 203k reads/s，差异来自旧时钟）。
- gzip reader：约 0.893M reads/s，269.8 MiB/s（按未压缩字节）。
- gzip-6 writer（含 `finish()`，系统 zlib）：19.46 s，约 51.4k reads/s，15.5 MiB/s，wall CV 1.81%。
- `run_benchmarks.py --filter '^benchmarkReader$'`：wall CV 3.18%，CPU CV 3.21%，`time_source=steady_clock`。
- gzip 输出 filter 1→4 线程：47.7k→46.2k reads/s（不扩展）；plain 输出 1→4 线程：1.65M→3.34M reads/s（约 2x）。

命令：

```bash
cmake -S . -B build/clang-release -DBUILD_BENCHMARKS=ON
cmake --build build/clang-release --target benchmarks benchmarks_concurrency -j 12
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release --output-dir /tmp/fq-bench --repetitions 2 \
  --filter '^benchmarkReader$'
```
