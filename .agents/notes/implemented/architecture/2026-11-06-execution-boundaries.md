# Agent Note: 执行会话语义与 writer 职责拆分

Status: implemented

## Problem

`fastq_writer.cpp` 同时承担临时文件发布、记录序列化、单流 gzip、块级并行调度四种职责，文件超过 800 行；新增块级 gzip 后继续在此文件叠加逻辑，review 和测试定位都会变差。

执行层则有两个后端各自复制同一段 `processBatch → write → afterCommit → merge` 顺序。`ExecutionRuntime::execute` 对 Adapter 的要求只是隐式 duck typing：缺方法或返回类型不匹配时，错误会在模板实例化深处出现；两个后端也可能在计量、归约或 `finish()` 时机上悄悄漂移。

## Decision

- 新增内部 `BatchOperationRunner`（`src/processing/execution_backend.*`），把批处理/提交/归约顺序固化为一处。Sequential 与 oneTBB 后端只负责调度，不再各自实现写入与合并。
- 新增 C++23 `BatchOperationAdapter` concept（`src/processing/execution_runtime.h`），编译期校验 `result_type`、`makeResult()`、`processBatch()`、`afterCommit()`、`merge()` 协议；`processBatch()` 的并发调用边界写入契约注释。
- 把 `GzipBlockWriter` 从 `fastq_writer.cpp` 拆到 `src/io/gzip_block_writer.{h,cpp}`，保持“多 member + 有界队列 + 独立写线程”的既有算法与公开行为不变。
- `FastqWriter` 的压缩模式解析、级别/块参数校验、输出缓冲预留在创建临时文件或 fd 之前完成；非法配置不再留下半初始化资源。临时文件与特殊文件两条路径共用同一套校验。
- 特殊文件判定下沉到 `src/io/file_target.h`，`FastqWriter` 与统计报告写出共用，删除两份同义实现。

## Alternatives considered

- **把 `ExecutionBackend` 模板化到 Adapter 类型** — 被否：每个 Adapter 会生成一套后端代码，编译体积和复杂度上升，运行时没有收益；现有类型擦除每批只发生一次。
- **抽象成格式无关的 `BatchSource`/`BatchSink` 框架** — 被否：项目刻意收窄为 FASTQ 工具，没有第二种数据格式的真实调用方；这与“不为假设扩展预留 API”的约束冲突。
- **继续保留 writer 单体，只加注释分区** — 被否：块级压缩的线程生命周期与文件发布协议已经是两个可独立验证的关注点，注释不能降低 review 成本。
- **为 gzip 块调度引入通用 codec 接口** — 暂否：当前只有 gzip 一种块压缩实现，通用接口没有第二个消费者；先保持 gzip 专用模块。

## Consequences

- 公共 API、CLI 参数、输出字节与错误语义不变；新增头文件都在 `src/`，不安装、不导出。
- `BatchOperationRunner::process()` 是 `const`，但只允许调用线程安全的 `ExecutionOperation::processBatch()`；`commit()`/`finish()` 只允许串行 stage 调用。
- oneTBB 后端的计量字段改为只在 `serial_in_order` 提交阶段更新，不再需要原子计数；pipeline 结束后的 reader/writer mutex 屏障保留，用于 TSan happens-before。
- `GzipBlockWriter` 仍是 gzip 专用内部模块；后续若出现第二种块压缩格式，再评估是否抽取 codec seam。
- `FastqWriter` 构造失败时的清理顺序固定为“停压缩线程 → 关 fd → 关 sync fd → 删临时文件”，避免 worker 写入已关闭 fd。

## Verification

```bash
./scripts/core/lint format
./scripts/core/lint check
./scripts/core/lint cppcheck -b build/clang-release
./scripts/core/build --preset clang-release --no-lto
ctest --test-dir build/clang-release --output-on-failure   # 14/14
./scripts/core/build --sanitizer asan --no-lto
ctest --test-dir build/clang-asan --output-on-failure      # 14/14
./scripts/core/build --sanitizer tsan --no-lto
ctest --test-dir build/clang-tsan --output-on-failure      # 14/14
```

`clang-tidy` 本机未安装，未运行；格式检查与三套构建/测试均通过。
