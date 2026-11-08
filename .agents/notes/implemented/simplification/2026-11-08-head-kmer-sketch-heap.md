# Agent Note: 头部 k-mer 草图改为固定槽位与惰性最小堆

Status: implemented

## Problem

`stat` 的头部 k-mer signature 使用容量 64 的有界 Space-Saving 草图。旧实现直接
操作 `std::map<std::string, uint64_t>`：命中时 `find` + 自增；满表时每遇到一个
新 key 都做一次 O(64) 的 `min_element` 扫描并 erase/emplace 一个树节点。read
头部互异时（测序数据常见），等于每条 read 一次 64 项字符串比较加两次节点分配。

## Decision

- `std::array<Slot, 64>` 持有 key 与计数，槽位字符串就地 `assign` 复用。
- `std::unordered_map<std::string, uint16_t>`（透明哈希）提供 key → 槽位索引。
- `std::vector` 维护惰性最小堆 `(count, slot)`：计数变化时入堆；取最小值前用
  `entries[slot].count == entry.count` 丢弃过期条目，堆超过 512 项时按当前槽位
  重建。
- 键比较只发生在索引查找与插入，堆比较是整数。语义保持 Space-Saving：满表时
  新 key 替换计数最小者并继承 `min + 1`；并列最小按槽位号取，结果确定。
- 每批结束用 `flushTo()` 一次性写入 `FqStatisticResult::headKmerCounts`，合并与
  报告排序路径不变。
- 头部 k-mer 提取改用 `string_view::substr`，去掉每条 read 的临时 string。

## Verification

- `tests/unit/statistics/test_statistics.cpp`：原有淘汰/累积用例不变；新增
  2000 个互异 kmer + 反复出现 key 的用例，断言仍为 64 条、无空 key、高频 key
  在表内且计数 ≥ 500。
- `./scripts/core/build --dev`、`./scripts/core/test --unit`、
  `./scripts/core/lint format` 均通过。

## Alternatives considered

- **每批精确累计再截断**：去掉 O(64) 扫描最简单，但会放弃 Space-Saving 的流式
  近似语义，且批内内存随互异 key 数增长，与"有界草图"的定位相反。
- **堆条目复制 key、按字典序打破并列**：能保持和旧 `min_element` 完全相同的
  并列选择，但每条 read 都要复制一次 20 字符的 k-mer，分配成本正是要消除的。
  槽位号并列同样确定，且测试不依赖并列顺序。
- **维持现状**：64 项扫描不算灾难，但用户明确把该路径列为性能待办，且改动只
  影响内部草图实现、公共 API 与输出格式不变。

## Consequences

- 每条 read 的草图更新从"O(64) 扫描 + 两次 map 节点操作"降为 O(log 64) 的整数
  比较；命中路径不再分配内存。
- 草图仍为 64 条上限，`operator+=` 与报告排序逻辑无需改动；`headKmerCounts`
  公共类型不变。
- 惰性堆最多保留 512 个条目（约 8 KiB），超过后重建，内存有界。
