// 多 gzip member 块级并行写出器。仅供 src 内部使用，不安装。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace fq::io::detail {

/**
 * @brief 有序块级 gzip 写出器
 * @details 每个块独立 deflate 成完整 gzip member，按提交顺序拼接；解压结果与单流一致。
 *          压缩 worker 只处理块，独立 writer 线程按序消费，主线程只负责提交。
 * @note 非线程安全的外部接口：submit()/finishAll() 由调用方串行调用。
 */
class GzipBlockWriter final {
public:
    GzipBlockWriter(int fd, std::string path, size_t threadCount, int compressionLevel);
    ~GzipBlockWriter();

    GzipBlockWriter(const GzipBlockWriter&) = delete;
    auto operator=(const GzipBlockWriter&) -> GzipBlockWriter& = delete;

    /// 提交一个未压缩块；队列满时阻塞形成背压。异常由 worker/writer 线程传播回来。
    void submit(std::vector<char> block);

    /// 等待所有已提交块写出，并停止/回收线程。可重复调用。
    void finishAll();

    [[nodiscard]] auto submittedBlocks() -> bool;

private:
    struct Task {
        std::uint64_t sequence = 0;
        std::vector<char> input;
    };

    static auto compressBlock(const std::vector<char>& input, int level) -> std::vector<char>;
    void workerLoop();
    void writerLoop();
    void rethrowWorkerErrorLocked();
    void writeCompressed(const std::vector<char>& compressed);
    void stopAndJoin();

    int fd_ = -1;
    std::string path_;
    int compressionLevel_ = 6;
    size_t maxInFlight_ = 2;
    off_t flushedOffset_ = 0;

    std::mutex mutex_;
    std::condition_variable workCv_;
    std::condition_variable spaceCv_;
    std::condition_variable resultCv_;
    std::deque<Task> workQueue_;
    std::map<std::uint64_t, std::vector<char>> ready_;
    std::uint64_t nextSubmit_ = 0;
    std::uint64_t nextWrite_ = 0;
    size_t inFlight_ = 0;
    bool stopRequested_ = false;
    bool stopped_ = false;
    std::exception_ptr workerError_;
    std::vector<std::thread> workers_;
    std::thread writerThread_;
};

}  // namespace fq::io::detail
