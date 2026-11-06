# Agent Note: 处理选项与 FASTQ 输出选项分离

Status: implemented

## Problem

`ProcessingOptions` 同时承载 batch/线程/内存预算和 `gzipBlockBytes`。这带来两个边界问题：

- `stat` 没有 FASTQ 输出，但共享的 `CommonCliOptions` 会产生 gzip 输出字段；`stat` 只能在命令层额外拒绝 `--gzip-block-mb`，类型本身仍然暗示一个不存在的输出阶段。
- 库调用方必须把 gzip 输出配置混进处理配置，`Pipeline` 无法表达“输出特性只在有 FASTQ 输出时生效”。后续增加压缩格式、输出缓冲或发布策略时，处理配置会继续膨胀。

gzip 块大小还会参与运行时内存预算，因此不能简单把它留在 CLI 层；它需要进入 `ResolvedRuntimeConfig`，但入口应当独立。

## Decision

- `ProcessingOptions` 只保留处理侧配置：`batchSize`、`threadCount`、`ProcessingProfile`、`memoryLimitBytes`、`batchCapacityBytes`。
- 新增公共 `fq::processing::OutputOptions`，当前包含 `gzipBlockBytes`；`0` 表示串行单流，非 0 时限制在 `[1 KiB, 1 GiB]`。验证逻辑从 `ProcessingOptions::validate()` 移到 `OutputOptions::validate()`。
- `resolveRuntimeConfig(options, output, hasCustomReader, hasCustomWriter)` 同时验证两组选项，并仍然是所有派生运行时参数的唯一入口；gzip 固定预算扣除逻辑不变。
- `ExecutionRuntimeRequest` 增加 `output` 字段；`Pipeline` 增加 `setOutputOptions()`，默认值为零配置。
- CLI 在 `CommonCliOptions` 中保留 `--gzip-block-mb` 的解析，但拆成 `toProcessingOptions()` 与 `toOutputOptions()`；`FilterPlan` 分别保存并注入 pipeline。`stat` 继续显式拒绝该参数。
- 这是 source-breaking 公共 API 变更：`ProcessingOptions::gzipBlockBytes` 被删除，调用方需显式构造 `OutputOptions` 并调用 `Pipeline::setOutputOptions()`。

## Alternatives considered

- **保留 `ProcessingOptions::gzipBlockBytes` 作为兼容别名** — 被否：会留下两个配置入口和两套验证，无法解决 `stat` 类型上“接受不存在输出配置”的问题；项目还没有需要长期兼容的稳定 ABI 约束。
- **把 gzip 配置放回 `FastqWriterOptions`，运行时完全不接收** — 被否：`Pipeline` 自己创建 writer，调用方无法传入 writer 层配置；直接暴露 writer 类型也会破坏自定义 Writer/pipeline 的抽象。
- **新增通用 `OutputPipelineOptions` / codec 层级** — 暂否：当前只有 gzip 块级并行一个真实输出特性，抽象层级会超过现有调用方需求。
- **让 `stat` 静默忽略 `--gzip-block-mb`** — 被否：参数看起来有效但完全不产生压缩输出，且会误触发块预算校验；显式拒绝比静默无效更安全。

## Consequences

- `ProcessingOptions` 的语义更窄，`OutputOptions` 只在有 FASTQ 输出时由 `Pipeline` 消费；`stat` 的 `StatisticOptions` 不包含输出配置。
- `ResolvedRuntimeConfig` 仍是内部唯一派生结果，gzip worker 数继续等于 `threadCount`，内存上限仍先扣除 writer 固定预算。
- CLI 行为保持：`filter --gzip-block-mb` 仍生效，`stat --gzip-block-mb` 仍以 `gzip-block-mb is not supported` 失败，默认输出字节不变。
- 迁移成本集中在库 API；现有 C++ 调用方如果只设置处理参数，无需改动，只有显式使用 `gzipBlockBytes` 的调用方需要拆分。

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

单元测试覆盖 `OutputOptions` 传播、worker 数与内存预算拒绝、CLI 转换和 `FilterPlan` 注入；`ExecutionRuntime` 用例验证输出配置确实到达默认 `FastqWriter`。安装包消费测试通过 `setOutputOptions()` 编译验证公共头传播。
