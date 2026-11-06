# Benchmark — 性能基准测试

基于 [Google Benchmark](https://github.com/google/benchmark) 的性能基准测试套件。

## 架构说明

本目录的 benchmark 代码与 `src/benchmark/` 是**互补关系**，分工如下：

| 目录 | 角色 | 内容 |
|------|------|------|
| `src/benchmark/` | 内部支撑库（`fq_benchmark` 静态库） | 数据收集器（`DataCollector`）、结果存储（`ResultStorage`）、共享类型定义 |
| `tools/benchmark/` | 可执行文件 | 基于 Google Benchmark 的具体测试用例（`filter_benchmark`、`stat_benchmark` 等） |

`benchmark_main.cpp` 提供自定义 main，在运行前通过 `DataCollector::collectSystemMetadata()` 将 git commit、CPU 型号、编译器版本等系统元数据注入 Google Benchmark JSON 输出的 `context` 字段，使每次基准结果自文档化。`fq_benchmark` 不作为公共 API 导出。

## C++ 基准测试

| 文件 | 测试内容 |
|------|----------|
| `fastq_io_benchmark.cpp` | FASTQ plain/gzip Reader、Writer 吞吐（Writer 含 `finish()` 发布路径） |
| `filter_benchmark.cpp` | Filter 命令端到端性能 |
| `stat_benchmark.cpp` | Stat 命令统计计算性能 |
| `pipeline_benchmark.cpp` | Sequential / oneTBB 公平对照 |
| `concurrency_benchmark.cpp` | 真实 Pipeline 的线程数/batch 大小扫描，含 gzip 输出变体 |
| `object_pool_benchmark.cpp` | ObjectPool 对象池分配性能 |

### 构建与运行

```bash
# 构建所有基准测试
cmake --build build --target benchmarks

# 运行单个基准测试
./build/tools/benchmark/benchmark_fastq_io --benchmark_format=json

# 运行并发扩展矩阵（1/2/4/8 线程 × 1K/10K/50K batch）
./build/tools/benchmark/benchmark_concurrency \
  --benchmark_format=json \
  --benchmark_out=/tmp/fastqtools-concurrency.json

# 构建线程扩展矩阵（不进入默认套件）
cmake --build build/clang-release --target benchmarks_concurrency

# 运行所有基准测试（1M×150 bp、5 次重复、JSON + wall/CPU/CV 摘要）
cmake --build build/clang-release --target run_benchmarks

# 如确实要把探索性矩阵也交给 runner，显式加开关
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release --output-dir /tmp/fq-exploratory \
  --repetitions 2 --include-exploratory

# 完整基准测试（5 次重复，输出 raw JSON、reads/s、MiB/s、峰值内存和 CV）
cmake --build build/clang-release --target benchmark_full

```

## 测量口径

- 每个 benchmark 在 harness 内用 `std::chrono::steady_clock` 记录 wall time，写入 `wall_seconds`；
  同时由 Google Benchmark 保留 `cpu_time`。`run_benchmarks.py` 用 `wall_seconds`、`read_count`、
  `input_bytes` 自行计算 reads/s 与 MiB/s，不再用可能被 WSL2 放大的 `real_time`。
- `wall_seconds` / `read_count` / `input_bytes` 都是**每次迭代**的值；重复运行先取中位数，再算 CV。
- Writer benchmark 在计时循环内调用 `FastqWriter::finish()`，因此包含 gzip close、`fsync` 和原子 rename；
  这与生产 `filter` 路径一致。
- `benchmark_concurrency` 不属于默认 `run_benchmarks`，用于固定机器上的线程扩展探索；它的
  `gzip_output` 场景专门验证 gzip 写出是否随线程数扩展。runner 默认排除该目标，显式
  `--include-exploratory` 才纳入。
- `benchmarkReaderGzip` 默认使用当前构建的 writer 生成的 gzip 流。跨 zlib/zlib-ng 版本比较 reader 时，
  设置 `FQ_BENCHMARK_GZIP_PATH` 指向同一份 gzip 文件；JSON 中 `fixed_stream=1`，
  context 的 `gzip_input_source=fixed`。否则压缩流大小/结构会污染 reader 倍数。

跨构建固定流 A/B 示例：

```bash
FQ_BENCHMARK_GZIP_PATH=/tmp/fq-fixed.fastq.gz \
  ./build/clang-release/tools/benchmark/benchmark_fastq_io \
  --benchmark_filter='benchmarkReaderGzip' --benchmark_min_time=0.001s \
  --benchmark_repetitions=1 --benchmark_format=json \
  --benchmark_out=/tmp/fixed.json
```
- 旧报告没有 `wall_seconds` 时，runner 回退到 `cpu_time`，并标记 `time_source=cpu_time`；不要用旧报告的
  `median_time` 与新报告做绝对比较。

## 分析脚本（`scripts/` 子目录）

| 脚本 | 用途 |
|------|------|
| `report_generator.py` | 从 JSON 结果生成 Markdown 报告和 SVG 图表 |
| `regression_detector.py` | 对比两次测试结果，检测性能回归（10% 警告 / 20% 严重） |
| `badge_generator.py` | 生成 shields.io 性能徽章 |
| `gen_benchmark_data.py` | 生成多规模、多参数的 FASTQ 测试数据集 |
| `run_baseline.sh` | 端到端基线测试脚本（多线程 × 多过滤条件） |

### 依赖

- **Python 3.8+**
- **matplotlib**（可选，用于图表生成）

## CMake 目标

| 目标 | 说明 |
|------|------|
| `benchmarks` | 构建默认基准测试可执行文件 |
| `benchmarks_concurrency` | 构建线程扩展矩阵（gzip 输出等） |
| `run_benchmarks` | 运行基准测试并输出 JSON |
| `benchmark_full` | 完整测试（3 次重复，聚合结果） |
| `benchmark_ci` | CI 模式（控制台 + JSON 双输出） |
| `benchmark_backend_comparison` | 7 次重复的 backend p50/p95、吞吐与峰值 RSS 对照 |

`benchmark_concurrency` 不属于默认 `run_benchmarks` 套件。它用于探索线程扩展性、
batch 大小敏感性和空算子路径，结果应在固定机器、固定输入和固定构建配置下单独归档。
