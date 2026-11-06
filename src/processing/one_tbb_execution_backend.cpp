#include <any>
#include <memory>
#include <mutex>
#include <utility>

#include "processing/execution_backend.h"
#include "processing/fastq_batch_pool.h"
#include <tbb/parallel_pipeline.h>
#include <tbb/task_arena.h>

namespace fq::processing {

namespace {

class OneTbbExecutionBackend final : public ExecutionBackend {
public:
    auto execute(ExecutionBackendContext context, ExecutionOperation& operation)
        -> ErasedExecutionOutcome override {
        using BatchResult = std::pair<std::shared_ptr<fq::io::FastqBatch>, std::any>;

        auto batchPool = createFastqBatchPool(context.config.maxLiveTokens,
                                              context.config.maxLiveTokens * 2,
                                              context.config.batchCapacityBytes,
                                              context.batchSize);
        BatchOperationRunner runner(context, operation);

        // serial_in_order 保证互斥；显式锁同时为 TSan 建立可见的 happens-before。
        std::mutex readerMutex;
        std::mutex writerMutex;

        // task_arena 提供实例级并行度隔离：同进程并发执行多个 pipeline
        // （不同 threadCount）互不干扰。global_control 是进程级的，
        // 多个活跃实例取最小值，会互相压制并行度。
        tbb::task_arena arena(static_cast<int>(context.config.threadCount));

        arena.execute([&] {
            tbb::parallel_pipeline(
                context.config.maxLiveTokens,
                tbb::make_filter<void, std::shared_ptr<fq::io::FastqBatch>>(
                    tbb::filter_mode::serial_in_order,
                    [&context, batchPool, &readerMutex](
                        tbb::flow_control& flow) -> std::shared_ptr<fq::io::FastqBatch> {
                        auto batch = batchPool->acquire();
                        {
                            std::lock_guard lock(readerMutex);
                            if (context.reader->nextBatch(*batch, context.batchSize)) {
                                return batch;
                            }
                        }
                        flow.stop();
                        return nullptr;
                    }) &
                    tbb::make_filter<std::shared_ptr<fq::io::FastqBatch>, BatchResult>(
                        tbb::filter_mode::parallel,
                        [&runner](std::shared_ptr<fq::io::FastqBatch> batch) -> BatchResult {
                            auto partial = runner.process(*batch);
                            return {std::move(batch), std::move(partial)};
                        }) &
                    tbb::make_filter<BatchResult, void>(
                        tbb::filter_mode::serial_in_order,
                        [&runner, &writerMutex](BatchResult batchResult) {
                            std::lock_guard lock(writerMutex);
                            runner.commit(*batchResult.first, std::move(batchResult.second));
                        }));
        });

        // pipeline 完成后显式建立 reader/writer 与调用线程之间的内存序。
        {
            std::lock_guard readerLock(readerMutex);
            std::lock_guard writerLock(writerMutex);
        }

        runner.finish();
        return std::move(runner).takeOutcome();
    }
};

}  // namespace

auto createOneTbbExecutionBackend() -> std::unique_ptr<ExecutionBackend> {
    return std::make_unique<OneTbbExecutionBackend>();
}

}  // namespace fq::processing
