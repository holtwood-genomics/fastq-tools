# Agent Note: CLI `--threads auto` 默认并行策略

Status: implemented

## Problem

CLI `--threads` 默认 1，`resolveRuntimeConfig` 只有 `threadCount > 1` 才进入 oneTBB；因此普通用户直接运行 `stat`/`filter` 时，并行流水线实际默认关闭。对一个以并行流水线为卖点的工具，这会让默认路径无法体现核心能力。

## Decision

- CLI `--threads` 默认 `auto`，接口仍接受 `auto|N`。
- `auto` 的解析规则：
  - stdin：`min(hardware_concurrency, 4)`；
  - 普通文件 `<64 MiB`：1 线程；
  - 其他文件：`min(hardware_concurrency, 8)`。
- 库 API `ProcessingOptions::threadCount` 仍默认 1，保持显式控制；`ProcessingOptions::validate` 继续拒绝 0，并拒绝超过 1024 的线程数（批次池按 `threadCount*2` 个批工作集预热，超大值在受限地址空间下会直接 `bad_alloc`，应作为参数错误在入口拒绝）。
- `-t 1` 强制串行，`-t N` 强制 N 线程；自定义 Reader/Writer 的保守 Sequential 规则保持不变。

小文件阈值来自实测：49 MiB 输入下 auto 比 `-t 1` 慢约 12%，峰值 RSS 高约 6 倍；1M×150 bp（303 MiB）下 `stat` auto 0.23 s vs `-t 1` 0.75 s。

## Alternatives considered

- **默认 `hardware_concurrency()`，不做阈值** — 被否：小文件和 stdin 会承担 TBB 启动/token 内存成本；200k reads 样本上 RSS 从 11 MB 升到 69 MB。
- **默认固定 4 线程** — 被否：大机器上浪费并行度，小机器上仍超订；不能表达“按输入规模回退”。
- **只改库 API 默认值** — 被否：库用户需要显式控制；CLI 才是面向普通用户的默认路径。
- **解析 gzip 解压后大小再决定** — 暂不做：需要额外解压/索引，成本高于收益；当前按压缩文件大小判断，极高压比文件可能保守地留在单线程，用户可用 `-t` 覆盖。

## Consequences

- **收益**：大文件默认吃到并行；`stat` 在 1M 数据上约 3.26x，plain filter 约 1.19x；小文件保持低内存。
- **代价与已知上限**：默认 RSS 上升（大文件约 86 MB）；gzip 输出不会因线程数提升而扩展，仍受串行压缩限制；64 MiB 阈值是经验值，后续应按真实文件分布重评。
- **兼容性**：输出保持逐字节/逐字段一致；`-t 1` 可回退到旧行为。CLI 帮助与 `docs/cli-reference.md` 已同步。

## Verification

```bash
# 1M×150 bp，stat：auto vs 串行
./build/clang-release/FastQTools stat -i 1m.fastq -o auto.stats
./build/clang-release/FastQTools stat -i 1m.fastq -o t1.stats -t 1
cmp auto.stats t1.stats

# 200k reads / 49 MiB：auto 应回退单线程
/usr/bin/time -f '%e s %M KB' ./build/clang-release/FastQTools filter \
  -i 200k.fastq -o out.fastq --min-quality 20 --min-length 100

# CLI 解析回归
./build/clang-release/tests/unit/test_cli --gtest_filter='CommonCliOptionsTest.*'
```
