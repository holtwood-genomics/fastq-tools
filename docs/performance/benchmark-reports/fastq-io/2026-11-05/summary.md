# zlib-ng 链接修复：gzip writer A/B 与 reader 口径复核

| 字段 | 值 |
|------|----|
| 日期 | 2026-11-05 |
| 工具 | Google Benchmark 1.9.5，`run_benchmarks.py` 2 次重复 |
| 构建 | Clang 18.1.3 Release，CMake 3.28.3，`ENABLE_LTO=OFF` |
| 数据集 | 1M reads × 150 bp，seed=42，316,888,890 bytes |
| 压缩库 | 系统 zlib 1.3 vs zlib-ng 2.3.3（`ZLIB_COMPAT` / `WITH_GZFILEOP`） |
| 基线 | 本机临时 A/B；偏离 [`environment.md`](../../../environment.md)（Ryzen 7 8845HS / 12 vCPU / WSL2） |

## 结论

同机、同源码、两份临时构建交替运行，gzip-6 writer（batch API，含 `finish()`）从系统 zlib 的 18.36–18.60 s 降到 zlib-ng 的 7.13–7.16 s，约 2.6x。writer 写的是同一份 plain FASTQ，归因较干净。

gzip reader 新增固定同一流的 A/B（`FQ_BENCHMARK_GZIP_PATH`，5 轮交替）：在 zlib-ng 生成的 150,622,757 B 流上，系统 zlib 841.9 ms → zlib-ng 574.7 ms（约 1.47x，CV 3.38%/2.46%）；在系统 zlib 生成的 156,815,211 B 流上，1,054.5 ms → 732.2 ms（约 1.44x，CV 2.29%/7.27%）。直接 benchmark 的 921→555 ms（1.65x）混入了流结构差异；`stat -t 1` 的 1.50–1.51 s → 1.25–1.26 s（1.2x）被统计计算稀释。结论：reader 固定流收益约 1.44–1.47x。

## 关键数据

### gzip-6 writer，A/B 交替

| 轮次 | 系统 zlib | zlib-ng compat |
|------|----------:|---------------:|
| 1 | 18.364 s / 54,453 reads/s / 16.5 MiB/s | 7.126 s / 140,340 reads/s / 42.4 MiB/s |
| 2 | 18.597 s / 53,773 reads/s / 16.3 MiB/s | 7.164 s / 139,593 reads/s / 42.2 MiB/s |

### gzip reader，固定同一流 A/B（5 轮交替）

| 流 | 系统 zlib | zlib-ng compat | 倍数 |
|----|----------:|---------------:|-----:|
| zlib-ng 流 150,622,757 B | 841.9 ms / 1.188M reads/s / 359.0 MiB/s | 574.7 ms / 1.740M reads/s / 525.8 MiB/s | 1.47x |
| 系统 zlib 流 156,815,211 B | 1,054.5 ms / 0.948M reads/s / 286.6 MiB/s | 732.2 ms / 1.366M reads/s / 412.7 MiB/s | 1.44x |

### gzip reader，旧口径对照

| 测量 | 系统 zlib | zlib-ng compat | 说明 |
|------|----------:|---------------:|------|
| `benchmarkReaderGzip`（2 次迭代） | 919/923 ms，1.088M/1.084M reads/s | 555 ms，1.80M reads/s | gzip 流由各自构建生成，混入流差异 |
| 同一旧流 `stat -t 1`（3 轮） | 1.50–1.51 s | 1.25–1.26 s | 同一 gzip 文件；stat 计算占比高 |

zlib-ng 的 `benchmarkReaderGzip` runner 摘要 wall CV 0.81%；writer A/B 单次迭代，不计算 CV。早期非交替测量为 writer 19.46 s → 7.89 s，方向与 A/B 一致。

## 口径边界

- writer：两次写同一份 plain FASTQ，属于干净对比；A/B 交替运行，避免负载/频率漂移。
- reader：固定流 A/B 是最干净口径；两条流分别得到 1.47x / 1.44x。直接 benchmark 的压缩流不同（156,815,211 B vs 150,622,757 B），1.65x 混入流差异；`stat -t 1` 的 1.2x 被统计计算稀释。
- 本快照来自 WSL2；绝对数值只用于本机同口径比较，不替代原生 Linux 的最终验收。

## 调用方式

```bash
# 系统 zlib 构建（临时代码：conan install 时 zlib_compat=False）
conan install build-config/dependencies --build=missing -of=/tmp/fq-system-test \
  -s build_type=Release -s compiler=clang -s compiler.version=18 \
  -o 'fastqtools/*:build_testing=False' -o 'fastqtools/*:build_benchmarks=True' \
  -o 'zlib-ng/*:zlib_compat=False' \
  -pr:h build-config/conan/profile-clang -pr:b default
cmake -S . -B /tmp/fq-system-cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/tmp/fq-system-test/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=ON -DENABLE_LTO=OFF
cmake --build /tmp/fq-system-cmake --target benchmark_fastq_io -j 12

# writer A/B（当前 zlib-ng 构建在 build/clang-release）
for bin in /tmp/fq-system-cmake/tools/benchmark/benchmark_fastq_io \
           ./build/clang-release/tools/benchmark/benchmark_fastq_io; do
  "$bin" --benchmark_filter='benchmarkWriter/gzip_6_batch' \
    --benchmark_min_time=1x --benchmark_repetitions=1 \
    --benchmark_format=json --benchmark_out=/tmp/writer-$(basename "$(dirname "$(dirname "$bin")")").json
done

# reader（当前 zlib-ng 构建）
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release --output-dir /tmp/fq-reader \
  --repetitions 2 --filter 'benchmarkReaderGzip'

# 固定流 A/B：两个构建设置同一个 FQ_BENCHMARK_GZIP_PATH
FQ_BENCHMARK_GZIP_PATH=/tmp/fq-fixed.fastq.gz \
  ./build/clang-release/tools/benchmark/benchmark_fastq_io \
  --benchmark_filter='benchmarkReaderGzip' --benchmark_min_time=0.001s \
  --benchmark_repetitions=1 --benchmark_format=json \
  --benchmark_out=/tmp/fixed-zlibng.json
```

## 原始数据

### 固定流 reader A/B（5 轮）

- `reader-fixed-zlibng-stream-system-r1..r5.json`：系统 zlib reader 读 zlib-ng 流
- `reader-fixed-zlibng-stream-zlibng-r1..r5.json`：zlib-ng reader 读 zlib-ng 流
- `reader-fixed-system-stream-system-r1..r5.json`：系统 zlib reader 读系统 zlib 流
- `reader-fixed-system-stream-zlibng-r1..r5.json`：zlib-ng reader 读系统 zlib 流

### writer A/B 与旧口径

- [writer-system-r1.json](./raw/writer-system-r1.json)
- [writer-zlibng-r1.json](./raw/writer-zlibng-r1.json)
- [writer-system-r2.json](./raw/writer-system-r2.json)
- [writer-zlibng-r2.json](./raw/writer-zlibng-r2.json)
- [reader-system-zlib.json](./raw/reader-system-zlib.json)
- [reader-writer-zlibng-runner.json](./raw/reader-writer-zlibng-runner.json)
- [reader-writer-zlibng-runner.summary.json](./raw/reader-writer-zlibng-runner.summary.json)
