#pragma once

#include "fqtools/io/interfaces.h"
#include "fqtools/processing/processing_options.h"

#include <any>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "processing/execution_backend.h"

namespace fq::processing {

/**
 * @brief ExecutionRuntime 的 Adapter 契约
 * @details Adapter 提供 result_type、makeResult()、processBatch()、afterCommit() 和 merge()；
 *          execute() 在编译期检查该协议，避免错误在模板实例化深处才暴露。
 * @note processBatch() 会在并行 stage 被并发调用，实现必须线程安全；
 *       afterCommit()/merge() 只会在串行提交阶段调用。
 */
template <typename Adapter>
concept BatchOperationAdapter = requires(Adapter& adapter,
                                         fq::io::FastqBatch& batch,
                                         typename Adapter::result_type& total,
                                         typename Adapter::result_type partial,
                                         std::uint64_t committedBytes) {
    { adapter.makeResult() } -> std::same_as<typename Adapter::result_type>;
    { adapter.processBatch(batch) } -> std::same_as<typename Adapter::result_type>;
    adapter.afterCommit(partial, committedBytes);
    adapter.merge(total, std::move(partial));
};

enum class ExecutionBackendPreference : std::uint8_t {
    Automatic,
    Sequential,
    OneTbb,
};

struct ExecutionRuntimeRequest {
    std::string inputPath;
    std::optional<std::string> outputPath;
    ProcessingOptions options;
    OutputOptions output;
    ExecutionBackendPreference backend = ExecutionBackendPreference::Automatic;
};

template <typename Result>
struct ExecutionRuntimeOutcome {
    Result result;
    ExecutionRuntimeMetrics metrics;
};

/**
 * @brief FASTQ 执行 runtime
 * @details 公共 interface 不暴露具体调度框架；后端选择和 I/O 创建均位于实现文件。
 */
class ExecutionRuntime {
public:
    ExecutionRuntime();
    ExecutionRuntime(std::unique_ptr<fq::io::IReader> customReader,
                     std::shared_ptr<fq::io::IWriter> customWriter = {});
    ~ExecutionRuntime();

    ExecutionRuntime(const ExecutionRuntime&) = delete;
    auto operator=(const ExecutionRuntime&) -> ExecutionRuntime& = delete;
    ExecutionRuntime(ExecutionRuntime&&) noexcept;
    auto operator=(ExecutionRuntime&&) noexcept -> ExecutionRuntime&;

    template <typename Adapter>
    requires BatchOperationAdapter<std::decay_t<Adapter>>
    auto execute(const ExecutionRuntimeRequest& request, Adapter&& adapter)
        -> ExecutionRuntimeOutcome<typename std::decay_t<Adapter>::result_type> {
        using Result = typename std::decay_t<Adapter>::result_type;

        class AdapterOperation final : public ExecutionOperation {
        public:
            explicit AdapterOperation(Adapter& value) : adapter_(value) {}

            auto makeResult() -> std::any override {
                return std::any(adapter_.makeResult());
            }

            auto processBatch(fq::io::FastqBatch& batch) -> std::any override {
                return std::any(adapter_.processBatch(batch));
            }

            void afterCommit(std::any& partial, std::uint64_t committedBytes) override {
                adapter_.afterCommit(std::any_cast<Result&>(partial), committedBytes);
            }

            void merge(std::any& total, std::any partial) override {
                adapter_.merge(std::any_cast<Result&>(total),
                               std::move(std::any_cast<Result&>(partial)));
            }

        private:
            Adapter& adapter_;
        };

        AdapterOperation operation(adapter);
        auto outcome = executeErased(request, operation);
        return {std::any_cast<Result>(std::move(outcome.result)), outcome.metrics};
    }

private:
    struct Impl;

    [[nodiscard]] auto executeErased(const ExecutionRuntimeRequest& request,
                                     ExecutionOperation& operation) -> ErasedExecutionOutcome;

    std::unique_ptr<Impl> impl_;
};

}  // namespace fq::processing
