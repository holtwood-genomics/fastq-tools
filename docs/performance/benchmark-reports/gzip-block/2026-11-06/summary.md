# gzip 块级并行 writer 矩阵（探索性）

| 字段 | 值 |
|------|----|
| 日期 | 2026-11-06 |
| 工具 | Google Benchmark 1.9.5，`benchmark_concurrency`（探索性，默认排除固定基线） |
| 构建 | Clang 18.1.3 Release，zlib-ng 2.3.3（`ZLIB_COMPAT` / `WITH_GZFILEOP`），`ENABLE_LTO=OFF` |
| 数据集 | 1M reads × 150 bp，seed=42，316,888,890 B；`loadBenchmarkBatches()` 常驻内存 |
| 基线 | 偏离 [`environment.md`](../../../environment.md)（Ryzen 7 8845HS / 12 vCPU / WSL2） |
| 机器 | AMD Ryzen 7 8845HS，12 vCPU，WSL2 |
| 口径 | writer-only，含 `finish()` 发布；`wall_seconds` |

## 结论

多 member gzip 块级并行在 writer-only 口径下可扩展到 8 个压缩 worker：1 MiB 块从 7.077 s（1t）降到 1.205 s（8t），约 5.9x；4 MiB 块从 7.085 s 降到 1.240 s，约 5.7x；16 MiB 块在 8t 时降到 1.463 s，约 4.9x。块越小，8t 扩展越好；16 MiB 在大并发下受内存带宽/缓存影响，收益下降。

## 矩阵

| 块大小 | 1t | 2t | 4t | 8t |
|-------:|---:|---:|---:|---:|
| 1 MiB | 7.077 s / 42.7 MiB/s | 3.641 s / 83.0 MiB/s | 1.972 s / 153.3 MiB/s | **1.205 s / 250.8 MiB/s** |
| 4 MiB | 7.085 s / 42.7 MiB/s | 3.777 s / 80.0 MiB/s | 2.142 s / 141.1 MiB/s | 1.240 s / 243.7 MiB/s |
| 16 MiB | 7.241 s / 41.7 MiB/s | 3.882 s / 77.9 MiB/s | 2.122 s / 142.4 MiB/s | 1.463 s / 206.6 MiB/s |

RSS（含约 316 MB 常驻数据集）：443–536 MiB；1 MiB/4t 约 494 MiB，1 MiB/8t 约 484 MiB。RSS 主要来自 benchmark 的 `loadBenchmarkBatches()`，不是 writer 的块缓冲上限。

单次迭代（`--benchmark_repetitions=1`），不计算 median/CV；这是探索性矩阵，只用于同机相对比较，不进入固定基线。

## 口径边界

- 这是 writer-only 基准，不含 reader/filter/pipeline 的端到端成本；端到端 `filter` 的 4t/8t 数据见 [`2026-11-06-gzip-block-parallel.md`](../../../optimizations/2026-11-06-gzip-block-parallel.md)。
- 输出是**多个完整 gzip member 按序拼接**，`gzip -t` / `zcat` / `FastqReader` 可读；压缩字节与串行单流不同，解压内容一致。
- 块大小是目标值：单条 read 大于块大小时，该块会增长到容纳整条 read。
- 本快照来自 WSL2；绝对数值只用于本机同口径比较，不替代原生 Linux 最终验收。

## 调用方式

```bash
./scripts/core/build --preset clang-release --no-lto
./build/clang-release/tools/benchmark/benchmark_concurrency \
  --benchmark_filter='benchmarkGzipBlockWriter' \
  --benchmark_min_time=0.001s --benchmark_repetitions=1 \
  --benchmark_format=json \
  --benchmark_out=docs/performance/benchmark-reports/gzip-block/2026-11-06/raw/benchmark_gzip_block.json
```

## 原始数据

- [benchmark_gzip_block.json](./raw/benchmark_gzip_block.json)
