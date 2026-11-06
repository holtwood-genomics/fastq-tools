# zlib-ng 链接修复：把声明的压缩库真正接到二进制

## 结论

项目在 `conanfile.py` 中声明了 `zlib-ng/2.3.3`，但实际二进制链接的是系统 `libz.so.1`。原因是 zlib-ng 默认关闭 `zlib_compat`，只安装 `zlib-ng.h` 与 `libz-ng`；项目却通过 `find_package(ZLIB)` 和 `<zlib.h>` 使用 zlib 兼容 API，CMake 因此静默找到了系统 zlib。

启用 `zlib_compat=True` 与 `with_gzfileop=True` 后，Conan 生成 `ZLIB::ZLIB` 目标，二进制改为静态链接 zlib-ng。同机、同 harness 下：

| 场景 | 系统 zlib | zlib-ng | 变化 |
|---|---:|---:|---:|
| gzip reader（固定 zlib-ng 流，5 轮） | 841.9 ms / 1.188M reads/s / 359.0 MiB/s | 574.7 ms / 1.740M reads/s / 525.8 MiB/s | 约 1.47x |
| gzip reader（固定系统 zlib 流，5 轮） | 1,054.5 ms / 0.948M reads/s / 286.6 MiB/s | 732.2 ms / 1.366M reads/s / 412.7 MiB/s | 约 1.44x |
| gzip reader（直接 benchmark，流不同） | 921 ms / 1.086M reads/s / 328.1 MiB/s | 555 ms / 1.80M reads/s / 544.2 MiB/s | 约 1.65x（混入流差异） |
| gzip-6 writer（A/B 交替，2 轮） | 18.36–18.60 s / 53.8–54.5k reads/s / 16.3–16.5 MiB/s | 7.13–7.16 s / 139.6–140.3k reads/s / 42.2–42.4 MiB/s | 约 2.6x |

writer 的 A/B 用同一份源码、同一台机器、两份临时构建交替运行，避免把负载/频率漂移算成库收益；两轮结果稳定。早期非交替测量得到 19.46 s → 7.89 s，方向与量级一致。

> **口径边界**：writer 是干净对比——两次都写同一份 plain FASTQ。reader 的早期两次基准各自使用当前构建写出的 gzip 文件：系统 zlib 产物 156,815,211 B，zlib-ng 产物 150,622,757 B；所以 1.65x 里混有压缩流结构差异。固定同一份系统 zlib gzip 文件，用两版 `FastQTools stat -t 1` 做 A/B：系统 zlib 1.50–1.51 s，zlib-ng 1.25–1.26 s，约 1.2x，但 stat 计算本身占比较高。
>
> 2026-11-05 夜间补充了 `FQ_BENCHMARK_GZIP_PATH` 固定流基准，两条流各跑 5 轮交替：zlib-ng 流 841.9→574.7 ms（1.47x，CV 3.38%/2.46%），系统 zlib 流 1,054.5→732.2 ms（1.44x，CV 2.29%/7.27%）。固定流 A/B 是最干净的口径；1.65x 混入了流差异，1.2x 被 stat 计算稀释。最终 reader 写 **约 1.44–1.47x**，不再写 2.0x。

## 发现过程

1. 旧 writer 基准的 gzip-6 路径只有约 15 MiB/s，先怀疑压缩级别或缓冲区。
2. `perf record` 显示约 63% 的周期落在 `libz.so.1.3` 内部，说明瓶颈确实在压缩库。
3. `ldd build/clang-release/FastQTools` 显示 `libz.so.1 => /lib/x86_64-linux-gnu/libz.so.1`；Conan 的 zlib-ng 包只有 `zlib-ng.h`、`libz-ng.a`，没有被链接。
4. 在临时构建中启用 `zlib-ng/*:zlib_compat=True` 后，`ldd` 不再出现 `libz.so.1`，gzip 读写吞吐同时提升。

## 决策

在 `build-config/dependencies/conanfile.py` 的 `configure()` 中显式设置：

```python
self.options["zlib-ng"].zlib_compat = True
self.options["zlib-ng"].with_gzfileop = True
```

这样 `find_package(ZLIB REQUIRED)` 会消费 Conan 生成的 `ZLIB::ZLIB`，继续使用现有的 `gzFile` / `gzwrite` / `gzread` 代码，不引入第二套压缩 API。`CMakeLists.txt` 还会检查该 target 是否暴露 `ZLIB_COMPAT`，不暴露时给出 warning，避免未来再次静默回退。

## 备选方案

- **改用 `zlib-ng::zlib-ng` + `zlib-ng.h` 的 `zng_*` API**：被否。需要改 reader/writer 全部调用点和类型，收益与兼容 API 相同，但扩大改动面。
- **保持系统 zlib，删掉未使用的 Conan 依赖**：被否。会放弃已声明的优化库，gzip 吞吐明显更低；且 README/架构文档一直声称使用 zlib-ng，删除等于承认能力回退。
- **只在本地构建命令加 option**：被否。CI、其他开发机和发布构建仍会静默回退，问题会复发；必须写进 recipe。

## 影响与边界

- **收益**：A/B 下 gzip-6 writer 约 2.6x；gzip reader 固定同一流约 1.44–1.47x（5 轮，两条流）。这是压缩库实现和指令集优化带来的收益，不是并行压缩。
- **代价**：首次 `conan install` 需要构建 zlib-ng compat 包；静态库会进入最终二进制，但 `ldd` 不再依赖系统 libz。
- **未改变**：gzip 仍是串行流；1→4 线程的 gzip 输出约 130.9k→134.9k reads/s，仍不随线程数扩展。块级并行压缩是后续独立改动。
- **输出字节变化**：zlib-ng 与系统 zlib 的压缩策略不同，相同输入/级别的 gzip 产物不再逐字节一致（本次 150,622,757 B vs 156,815,211 B），解压内容一致。依赖 gzip 文件 hash 的流水线需要重新基线化；这不是数据正确性问题，但属于可复现性边界。
- **可复现性**：conan.lock 仍锁定 zlib-ng recipe revision；compat 选项由 recipe 的 `configure()` 固定，不依赖调用者手工传参。

## 验证

原始 JSON 与完整口径见 [`benchmark-reports/fastq-io/2026-11-05`](../benchmark-reports/fastq-io/2026-11-05/summary.md)。

```bash
./scripts/core/build --preset clang-release --no-lto

# 二进制不应再动态链接系统 libz
ldd build/clang-release/FastQTools | grep -E 'libz|zlib' || true

# writer / reader 复测
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release \
  --output-dir /tmp/fq-zlibng-runner \
  --repetitions 2 \
  --filter 'benchmarkReaderGzip|benchmarkWriter/gzip_6_batch'

# gzip 产物仍可被系统工具读取
./build/clang-release/FastQTools filter -i /tmp/1m.fastq -o /tmp/out.fastq.gz -t 4
gzip -t /tmp/out.fastq.gz
zcat /tmp/out.fastq.gz | cmp - /tmp/1m.fastq
```

`run_benchmarks.py` 摘要：`benchmarkReaderGzip` 555.380 ms / 1,800,686.5 reads/s / 544.2 MiB/s，wall CV 0.81%。同一命令下的 `benchmarkWriter/gzip_6_batch` 为 7,889.985 ms / 126,758.1 reads/s / 38.3 MiB/s，wall CV 1.09%——这是**早期非交替运行**，仅作方向性记录；最终 writer 数据以上表的两轮 A/B 交替为准。

固定流 A/B 的命令：

```bash
# 两个构建都设置同一个固定流；JSON 中 fixed_stream=1
for bin in /tmp/fq-system-cmake/tools/benchmark/benchmark_fastq_io \
           ./build/clang-release/tools/benchmark/benchmark_fastq_io; do
  FQ_BENCHMARK_GZIP_PATH=/tmp/fq-fixed.fastq.gz "$bin" \
    --benchmark_filter='benchmarkReaderGzip' --benchmark_min_time=0.001s \
    --benchmark_repetitions=1 --benchmark_format=json \
    --benchmark_out=/tmp/fixed-$(basename "$(dirname "$(dirname "$bin")")").json
done
```

本次归档的两条固定流：zlib-ng 产物 150,622,757 B，系统 zlib 产物 156,815,211 B；5 轮交替中位数如上表。
