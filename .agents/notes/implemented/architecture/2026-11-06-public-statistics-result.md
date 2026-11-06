# Agent Note: 正式导出统计结果类型

Status: implemented

## Problem

`Pipeline::readStatistics()` 在公共头中返回 `const fq::statistics::FqStatisticResult&`，但 `FqStatisticResult` 只在 `src/statistics/fq_statistic.h` 中定义，公共头只做了前向声明。结果是：

- 库调用方可以拿到引用并传给 `writeStatisticsOutputs()`，但无法读取 `readCount`、位置质量分布等任何字段；
- 使用 `readStatistics()` 的公共工作流必须包含 `src/` 内部头，违反安装包只暴露 `include/` 的边界；
- `FqStatisticResult` 的字段和 `kMaxQual`/`kMaxBaseNum` 已经决定公开 API 的形状，却以“内部类型”的形式存在，头文件职责与实际契约不一致。

## Decision

- 新增公共头 `include/fqtools/statistics/statistics_result.h`，正式定义 `FqStatisticResult`、`kMaxQual` 与 `kMaxBaseNum`。
- `fqtools/statistics/interfaces.h` 直接包含该结果头，不再前向声明 `FqStatisticResult`；`Pipeline` 公共头也包含它，使单独包含 pipeline 头时返回值类型可用。
- `<fqtools/fq.h>` 聚合新头，安装包消费方无需知道 `src/statistics/fq_statistic.h`。
- 删除内部包装头 `src/statistics/fq_statistic.h`；内部实现和测试统一直接包含公共结果头或 `statistics/interfaces.h`。
- `FqStatisticResult` 的字段、`qualityAt()`/`baseAt()` 与 `operator+=` 保持现有语义；这是一次头文件归属修正，不改变结构布局或运行时逻辑。
- `ensureCapacity()` 在导出后补上公共契约边界：读长超过 `uint32_t` 可表示范围，或扁平化数组长度超过容器 `max_size()` 时抛 `std::length_error`，避免 `maxReadLength` 截断和 `newLength * kMaxQual` 溢出。

## Alternatives considered

- **继续保持不透明类型，只允许 `writeStatisticsOutputs()` 消费** — 被否：`readStatistics()` 的公共返回值无法使用，且文档承诺的 QC 指标对库用户不可见；内部头路径也不应成为受支持依赖。
- **新增一个只含汇总字段的 `StatisticSummary`，保留完整结果内部化** — 暂否：调用方仍需要位置分布和 signature/duplicate 明细来复用与 `stat` 相同的报告/分析；再包一层会增加转换和两套类型维护成本。
- **把 `fq_statistic.h` 复制一份到公共目录，保留两个定义** — 被否：重复类型会立即产生 ODR/维护分叉，不能作为长期接口。
- **只把前向声明改成定义但不删除内部头** — 被否：会留下两个看似等价的入口，后续修改容易漂移；直接删除内部包装头更清晰。

## Consequences

- `FqStatisticResult` 的字段与位置分布布局成为公共契约，后续修改需要按 API 变更处理；这是 `Pipeline::readStatistics()` 已经隐含承诺的成本。
- 公共头增加了 `<vector>`、`<map>`、`<functional>` 等标准库依赖，但类型本身就是结果数据，依赖与职责一致。
- 内部实现不再需要 `src/statistics/fq_statistic.h` 这一层间接；`fq_stat_core` 的编译与链接布局不变。
- 安装包消费测试现在会启用 `readStatistics()` 并直接读取 `readCount`/`totalBases`，同时验证类型可默认构造；它还通过安装后的公共头调用 `ensureCapacity()`、`qualityAt()`/`baseAt()` 与 `operator+=`，覆盖结果类型的完整消费路径和导出符号。

## Verification

```bash
./scripts/core/lint format
./scripts/core/lint check
./scripts/core/build --preset clang-release --no-lto
ctest --test-dir build/clang-release --output-on-failure
./scripts/core/build --sanitizer asan --no-lto
ctest --test-dir build/clang-asan --output-on-failure
./scripts/core/build --sanitizer tsan --no-lto
ctest --test-dir build/clang-tsan --output-on-failure
./scripts/core/notes verify
```

`tests/packaging/main.cpp` 只通过安装后的 `<fqtools/fq.h>` 使用 `fq::statistics::FqStatisticResult`，并访问其字段、位置指针与 `operator+=`；这比只在源码树内编译更能证明公共头边界和导出符号完整。单元测试额外覆盖超出 `uint32_t` 的读长被拒绝且不产生部分扩容。
