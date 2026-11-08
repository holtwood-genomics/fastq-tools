# Agent Note: 逐位置统计的固定内存上界

Status: implemented

## Problem

`FqStatisticResult` 按最长 read 分配 `posQualityDist`（42 槽/位置）与
`posBaseDist`（5 槽/位置），每碱基约 376 字节。一个极小的 gzip 文件解压出
超长 read 即可把内存放大到 GiB 级，`--memory-limit-gb` 也无法约束。

## Decision

- 逐位置分布最多保留 `kMaxPositionStats = 16384` 个位置，`ensureCapacity`
  只按 `min(readLength, cap)` 扩容，`maxReadLength` 仍记录真实最大长度。
- 新增 `baseTotals` / `qualTotals` 全局累计数组，覆盖超过位置上限的 read；
  `computeMetrics` 的 Q20/Q30、A/C/G/T/N 总数改用全局数组，逐位置行只遍历
  `positionCount()`。
- `operator+=` 同步合并两个全局数组。

## Alternatives considered

- **对超长 read 直接拒绝或截断整条 read**：能保住逐位置数组，但会改变 read 计数
  与碱基/质量总量，超出"统计"语义。改为总量完整累计、仅逐位置截断。
- **按最长 read 无上限分配**：与 `--memory-limit-gb` 的预算冲突，极小 gzip 输入
  仍可放大到 GiB 级；不采用。
- **只限制单条 read 长度**：会拒绝合法 ONT 超长 read，且不解决多批聚合的内存
  上界问题。

## Consequences

内存上界从 O(最长 read) 变为约 6 MB；超过 16384 bp 的 read 不再输出尾部逐位置
统计，但总量、Q20/Q30、碱基比例仍准确。ONT 超长 read 的逐位置曲线会被截断，
这是有意的取舍。

## Verification

- `./scripts/core/test`（Debug）与 `./scripts/core/test -t Release`：14/14 通过。
- 新增 `CapsPerPositionStatsForHugeReads` 断言容量上限与真实 maxReadLength。
