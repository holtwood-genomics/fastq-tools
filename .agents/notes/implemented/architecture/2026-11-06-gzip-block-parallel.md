# Agent Note: gzip 块级并行（多 member + 有界队列，opt-in）

Status: implemented

## Problem

zlib-ng compat 链接修复后，gzip writer 单流压缩从约 18.4 s 降到 7.1 s（1M×150 bp，A/B），但 `filter -o out.fastq.gz` 的 1→4 线程吞吐仍几乎不变（约 131k→135k reads/s）。瓶颈已经从“库实现”转移到“单流压缩在串行段”：pipeline 并行只加速 reader/filter，writer 仍按一个 `gzFile` 顺序压缩。

## Decision

- 新增 `FastqWriterOptions::gzipBlockBytes`（默认 0）与 `gzipThreads`（默认 1）；公共 `OutputOptions::gzipBlockBytes` 经 runtime 透传到 writer。
- CLI 新增 `--gzip-block-mb N`：0 = 现有串行单流（默认），>0 = 多 member 块级并行；worker 数取 `--threads`。
- 每个块独立 `deflateInit2(..., 15 + 16, ...)` 成完整 gzip member，按提交顺序拼接；解压内容与串行一致，压缩字节不同。
- 有界队列：最多 `gzipThreads + 2` 个已提交未写出的块；`submit()` 在队列满时阻塞形成背压。
- 独立 writer 线程按 `nextWrite_` 顺序消费 `ready_`；worker 只压缩，不直接写 fd。`finish()` 仍执行 fsync、原子 rename、目录 fsync；空输入写一个合法的空 member。
- 默认关闭，避免无提示改变输出字节与内存曲线；plain 输出、`-o -` 与块级 gzip 冲突时直接报配置错误。
- `stat` 没有 FASTQ 输出，显式拒绝 `--gzip-block-mb`，避免“静默无效 + 误触发块预算校验”。
- 公共输出配置后来从 `ProcessingOptions` 拆为 `OutputOptions`，见 [处理选项与 FASTQ 输出选项分离](./2026-11-06-output-options-boundary.md)。

## Alternatives considered

- **bgzip/pigz 依赖** — 被否：引入新依赖和 indexed 格式，超出“标准 gzip”范围；多 member 已能被 `gzip -d` / `zcat` / zlib 正确读取。
- **单流并行 deflate（共享字典）** — 被否：需要自己管理块边界、字典窗口和 trailer，复杂度和正确性风险高。
- **先缓存全部输出再并行压缩** — 被否：内存随输入线性增长，违背有界队列目标。
- **默认启用** — 暂否：输出字节与 RSS 曲线会变化，缺少真实数据分布验证；先 opt-in，等发布说明与更多数据。
- **TBB 任务压缩** — 被否：writer 在 pipeline 串行阶段调用，向同一 arena 提交并等待有死锁风险；独立 `std::thread` 池更可控。

## Consequences

- **收益**：端到端 `filter` 1M×150 bp，1 MiB 块 4t 2.16 s、8t 1.39 s；相对串行 7.8 s 约 3.6x/5.6x。writer-only 1 MiB 块 1t 7.077 s → 8t 1.205 s（约 5.9x）。
- **输出变化**：多 member gzip 的压缩字节与单流不同（1 MiB 约 150,762,212 B vs 串行 150,622,757 B），解压内容一致；依赖产物 hash 的流水线需重新基线化。
- **内存**：块缓冲上限约 `gzipBlockBytes × (gzipThreads + 4)`（`gzipThreads + 2` 个在途块 + 1 个正在写出 + 1 个正在填充；worker 压缩时输入/输出短暂并存，故为近似值）；`--memory-limit-gb` 已把该固定预算从 token 预算中扣除。
- **线程**：CLI `--threads N` 同时驱动 pipeline 与 gzip worker，峰值线程约 2N；12 vCPU 上 8t 仍扩展。gzip worker 数与 pipeline 线程数绑定，当前不能单独设置；低内存场景应整体降低 `--threads` 或减小块大小。
- **块大小是目标值**：单条 read 大于块大小时，该块会增长到容纳整条 read。
- **不是 bgzip**：没有随机访问索引；indexed gzip 场景仍用 bgzip/tabix。
- **踩坑**：第一版主线程同时提交和按序写出，`resultCv_.notify_one()` 在 writer 线程与 `finishAll()` 两个等待者间丢唤醒，4 MiB×4t 稳定死锁。改为独立 writer 线程 + `notify_all()` 后修复。

## Verification

```bash
./scripts/core/build --preset clang-release --no-lto
./scripts/core/test --build-dir build/clang-release

# 端到端 1M×150 bp
for t in 1 2 4 8; do
  ./build/clang-release/FastQTools filter -i /tmp/1m.fastq \
    -o /tmp/out-$t.fastq.gz --gzip-block-mb 4 -t $t
  gzip -t /tmp/out-$t.fastq.gz
  zcat /tmp/out-$t.fastq.gz | cmp - /tmp/1m.fastq
done

# writer-only 探索矩阵
./build/clang-release/tools/benchmark/benchmark_concurrency \
  --benchmark_filter='benchmarkGzipBlockWriter' \
  --benchmark_min_time=0.001s --benchmark_repetitions=1 \
  --benchmark_format=json --benchmark_out=/tmp/gzip-block.json
```

单元测试覆盖 9 个 gzip 块级场景：串行 vs 块级的**解压字节流完全一致**、1000 条记录跨多块保序、单条 read 大于块大小、`/dev/full` 的 ENOSPC 能传回调用线程且不挂死、空输出、非法块大小/worker/plain/stdout、未 finish 不发布、`/dev/null` 直接写。e2e 覆盖 CLI 接线（`filter --gzip-block-mb` 内容一致、`stat` 拒绝该选项）。16 次 CLI 输出（4 串行 + 12 块级）全部通过 `gzip -t` 与 `zcat | cmp`。

ASan 全量测试通过；TSan 全量测试（仓库 `build-config/sanitizers/tsan.supp`）14/14 通过，CLI 默认与块级路径均为 0 告警且无 `fastq_writer.cpp` 帧。裸 TSan（不加载抑制文件）会把未插桩 TBB 的 pipeline token 交接报成竞争，属依赖侧假阳性；不要据此改项目代码。
