#pragma once

#include "fqtools/io/interfaces.h"

#include <any>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "processing/resolved_runtime_config.h"

namespace fq::processing {

struct ExecutionRuntimeMetrics {
    std::uint64_t batchCount = 0;
    std::uint64_t committedBytes = 0;
};

struct ErasedExecutionOutcome {
    std::any result;
    ExecutionRuntimeMetrics metrics;
};

/**
 * @brief 批处理操作的类型擦除 seam
 * @details 每批仅进行一次虚调用与结果装箱，逐 read 热点不经过该接口。
 */
class ExecutionOperation {
public:
    virtual ~ExecutionOperation() = default;

    [[nodiscard]] virtual auto makeResult() -> std::any = 0;
    virtual auto processBatch(fq::io::FastqBatch& batch) -> std::any = 0;
    virtual void afterCommit(std::any& partial, std::uint64_t committedBytes) = 0;
    virtual void merge(std::any& total, std::any partial) = 0;
};

struct ExecutionBackendContext {
    std::shared_ptr<fq::io::IReader> reader;
    std::shared_ptr<fq::io::IWriter> writer;
    ResolvedRuntimeConfig config;
    size_t batchSize = 1;
};

/**
 * @brief 单次执行内的批处理/提交/归约会话
 * @details 把「processBatch → writer->write → afterCommit → merge」的唯一顺序固化在一处，
 *          Sequential 与 oneTBB backend 共用，避免计量、归约和提交时机在实现间漂移。
 *          process() 会被并行 stage 并发调用；commit()/finish() 只由串行 stage 调用。
 */
class BatchOperationRunner {
public:
    BatchOperationRunner(ExecutionBackendContext& context, ExecutionOperation& operation);

    [[nodiscard]] auto process(fq::io::FastqBatch& batch) const -> std::any;
    void commit(fq::io::FastqBatch& batch, std::any partial);
    void finish();
    [[nodiscard]] auto takeOutcome() && -> ErasedExecutionOutcome;

private:
    ExecutionBackendContext& context_;
    ExecutionOperation& operation_;
    std::any result_;
    ExecutionRuntimeMetrics metrics_;
};

/**
 * @brief 执行后端 seam
 * @details 隐藏具体调度框架，所有后端共享相同 I/O 与批处理契约。
 */
class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;

    [[nodiscard]] virtual auto execute(ExecutionBackendContext context,
                                       ExecutionOperation& operation) -> ErasedExecutionOutcome = 0;
};

[[nodiscard]] auto createSequentialExecutionBackend() -> std::unique_ptr<ExecutionBackend>;
[[nodiscard]] auto createOneTbbExecutionBackend() -> std::unique_ptr<ExecutionBackend>;

}  // namespace fq::processing
