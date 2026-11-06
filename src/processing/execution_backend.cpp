#include "processing/execution_backend.h"

#include "fqtools/io/fastq_io.h"

#include <utility>

namespace fq::processing {

BatchOperationRunner::BatchOperationRunner(ExecutionBackendContext& context,
                                           ExecutionOperation& operation)
    : context_(context), operation_(operation), result_(operation.makeResult()) {}

auto BatchOperationRunner::process(fq::io::FastqBatch& batch) const -> std::any {
    return operation_.processBatch(batch);
}

void BatchOperationRunner::commit(fq::io::FastqBatch& batch, std::any partial) {
    const auto committedBytes = context_.writer ? context_.writer->write(batch) : 0;
    operation_.afterCommit(partial, committedBytes);
    operation_.merge(result_, std::move(partial));
    ++metrics_.batchCount;
    metrics_.committedBytes += committedBytes;
}

void BatchOperationRunner::finish() {
    if (context_.writer) {
        context_.writer->finish();
    }
}

auto BatchOperationRunner::takeOutcome() && -> ErasedExecutionOutcome {
    return {std::move(result_), metrics_};
}

namespace {

class SequentialExecutionBackend final : public ExecutionBackend {
public:
    auto execute(ExecutionBackendContext context, ExecutionOperation& operation)
        -> ErasedExecutionOutcome override {
        BatchOperationRunner runner(context, operation);
        fq::io::FastqBatch batch(context.config.batchCapacityBytes, context.batchSize);

        while (context.reader->nextBatch(batch, context.batchSize)) {
            auto partial = runner.process(batch);
            runner.commit(batch, std::move(partial));
        }

        runner.finish();
        return std::move(runner).takeOutcome();
    }
};

}  // namespace

auto createSequentialExecutionBackend() -> std::unique_ptr<ExecutionBackend> {
    return std::make_unique<SequentialExecutionBackend>();
}

}  // namespace fq::processing
