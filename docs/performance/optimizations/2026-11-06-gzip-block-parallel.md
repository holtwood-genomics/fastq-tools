# gzip 块级并行：多 member + 有界队列（opt-in）

## 结论

在 zlib-ng compat 链接修复后，gzip writer 的单流压缩已经快了约 2.6x，但 1→4 线程仍不扩展：压缩本身在串行段，线程数只影响 reader/filter。2026-11-06 增加了一条**显式 opt-in** 的块级并行路径：把未压缩字节切成块，每块独立 deflate 成完整 gzip member，按提交顺序拼接写出。

端到端 `filter -o out.fastq.gz`（1M×150 bp，Ryzen 7 8845HS / 12 vCPU / WSL2）：

| 块大小 | 1t | 2t | 4t | 8t |
|-------:|---:|---:|---:|---:|
| 串行单流（默认） | 7.83 s | 8.05 s | 8.20 s | 7.99 s |
| 1 MiB | 7.14 s | 3.78 s | 2.16 s | **1.39 s** |
| 4 MiB | 7.88 s | 4.00 s | 2.18 s | 1.40 s |
| 16 MiB | 7.57 s | 4.16 s | 2.52 s | 1.66 s |

4t 约 3.6x、8t 约 5.6x（相对串行 7.8 s）；1–4 MiB 块在 4t/8t 基本持平，16 MiB 在大并发下更慢且内存更高。writer-only 矩阵（1 MiB 块 7.077→1.205 s，5.9x）见 [`benchmark-reports/gzip-block/2026-11-06`](../benchmark-reports/gzip-block/2026-11-06/summary.md)。

## 设计

- `FastqWriterOptions::gzipBlockBytes > 0` 启用；默认 `0` 保持现有单流 `gzdopen` 路径，输出字节与旧实现一致。
- `gzipThreads` 指定压缩 worker 数；CLI 用 `--gzip-block-mb` 启用，worker 数取 `--threads`。
- 块缓冲按 `gzipBlockBytes` 预留；块满时移交给 worker，主线程只做序列化和提交，不参与压缩。
- 每个块用 `deflateInit2(..., 15 + 16, ...)` 生成完整 gzip member；按序拼接是标准 gzip 多 member 流，`gzip -d` / `zcat` / zlib `gzread` 均可读。
- 有界队列：最多 `gzipThreads + 2` 个已提交未写出的块；`submit()` 在队列满时阻塞，形成背压。
- 一个专用写线程按 `nextWrite_` 顺序消费就绪块，保证输出顺序；worker 只负责压缩，不直接写 fd。
- `finish()` 仍走原来的 fsync/原子 rename/目录 fsync；空输入写出一个合法的空 gzip member（20 B），与串行 `gzclose` 一致。
- `--gzip-block-mb` 与 plain 输出或 `-o -` 冲突时直接报配置错误；特殊文件（如 `/dev/null`）走直接写路径，也支持块级并行。

## 踩坑：`notify_one` 丢唤醒导致死锁

第一版让主线程同时负责“提交块”和“按序写出就绪块”，并用一个 `resultCv_` 同时唤醒 writer 线程与 `finishAll()`。在 4 MiB 块 × 4 线程、约 500k reads 时稳定复现：进程所有线程 futex 等待，临时文件停在约一半或接近末尾。

根因是 `condition_variable::notify_one()` 在**两个不同等待者**之间竞争：worker 把块放进 `ready_` 后 `notify_one()`，可能唤醒正在等“全部写完”的 `finishAll()`；`finishAll()` 检查条件不满足后继续睡，而真正该消费该块的 writer 线程没有被唤醒。worker 随后空闲，writer 永远等 `ready_[nextWrite_]`。1 MiB/4t 因为通知更密集、偶然没踩到，4 MiB/4t 稳定触发。

修复分两步：

1. 去掉主线程的 `drainReady()`，改成独立的 writer 线程按序消费 `ready_`，主线程只提交；
2. worker 入队后改用 `resultCv_.notify_all()`，让 writer 和 `finishAll()` 都能重新检查谓词。

修复后 500k/1M × 4 MiB × 4t/8t 均正常完成，`gzip -t` 与 `zcat | cmp` 通过。

## 正确性与边界

- 16 次 CLI 输出（4 次串行 + 12 次块级：1/4/16 MiB × 1/2/4/8t）全部通过 `gzip -t` 与 `zcat | cmp`；解压内容与输入逐字节一致。
- 单元测试覆盖 9 个场景：串行 vs 块级的解压字节流完全一致、1000 条记录跨多块保序、单条 read 大于块大小、`/dev/full` 的 ENOSPC 传回调用线程且不挂死、空输入可读、非法块大小/worker 数/plain 输出/stdout 拒绝、未 `finish()` 析构不发布、`/dev/null` 直接写。
- ASan 全量测试通过；TSan（仓库 `build-config/sanitizers/tsan.supp`）下 14/14 通过，CLI 默认与块级路径 0 告警，`fastq_writer.cpp` 无竞争帧。
- **输出字节变化**：多 member 流的压缩字节与单流不同（1 MiB 块约 150,762,212 B，4 MiB 约 150,658,611 B，串行约 150,622,757 B），解压内容一致；依赖 gzip 产物 hash 的流水线需重新基线化。
- **`stat` 不接受该选项**：`stat` 没有 FASTQ 输出，命令行显式拒绝 `--gzip-block-mb`，避免静默无效和误触发块预算校验。
- **不是 bgzip**：没有随机访问索引；需要 indexed gzip 的场景仍应使用 bgzip/tabix。
- **默认关闭**：默认路径仍写单流，避免无提示地改变输出字节和内存曲线；是否默认启用需要更多真实数据与发布说明。
- **线程模型**：CLI 的 `--threads N` 同时驱动 pipeline 和 gzip worker，峰值线程数约为 2N；12 vCPU 上 8t 仍能扩展。gzip worker 数与 pipeline 线程数绑定，当前不能单独设置；低内存场景应整体降低 `--threads` 或减小块大小。
- **内存**：块缓冲上限约 `gzipBlockBytes × (gzipThreads + 4)`（`gzipThreads + 2` 个在途块 + 1 个正在写出 + 1 个正在填充；worker 压缩时输入/输出会短暂并存，故为近似值）；`--memory-limit-gb` 已把该固定预算从 token 预算中扣除，但 benchmark RSS 还包含常驻数据集。
- **块大小是目标值**：单条 read 大于块大小时，该块会增长到容纳整条 read。

## 备选方案（被否）

- **bgzip/pigz 依赖**：引入新依赖和 indexed 格式，超出“保持标准 gzip”的范围；当前多 member 已能让 `gzip -d` 正确解压。
- **单流并行 deflate（共享字典）**：需要自己管理块边界、字典窗口和 trailer，复杂度和正确性风险高；zlib 不提供现成接口。
- **先缓存全部输出再并行压缩**：内存随输入线性增长，违背有界队列目标。
- **默认启用**：会改变输出字节与内存曲线，缺少真实数据分布验证；先保持 opt-in。
- **直接用 TBB 任务压缩**：writer 在 pipeline 串行阶段被调用，向同一 arena 提交并等待有死锁风险；独立 `std::thread` 池更可控。

## 复现

```bash
# 端到端矩阵
for mb in 0 1 4 16; do
  for t in 1 2 4 8; do
    /usr/bin/time -f "%e s %M KB" ./build/clang-release/FastQTools filter \
      -i /tmp/1m.fastq -o /tmp/out-${mb}-${t}.fastq.gz --gzip-block-mb "$mb" -t "$t"
  done
done

# writer-only 矩阵（探索性）
./build/clang-release/tools/benchmark/benchmark_concurrency \
  --benchmark_filter='benchmarkGzipBlockWriter' \
  --benchmark_min_time=0.001s --benchmark_repetitions=1 \
  --benchmark_format=json --benchmark_out=/tmp/gzip-block.json

# 正确性
gzip -t /tmp/out-4-8.fastq.gz
zcat /tmp/out-4-8.fastq.gz | cmp - /tmp/1m.fastq
```
