#include "gzip_block_writer.h"

#include "fqtools/error/error.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

namespace fq::io::detail {

GzipBlockWriter::GzipBlockWriter(int fd, std::string path, size_t threadCount, int compressionLevel)
    : fd_(fd),
      path_(std::move(path)),
      compressionLevel_(compressionLevel),
      maxInFlight_(std::max<size_t>(2, threadCount + 2)) {
    try {
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        writerThread_ = std::thread([this] { writerLoop(); });
    } catch (...) {
        stopAndJoin();
        throw;
    }
}

GzipBlockWriter::~GzipBlockWriter() {
    stopAndJoin();
}

void GzipBlockWriter::submit(std::vector<char> block) {
    std::unique_lock lock(mutex_);
    spaceCv_.wait(lock, [this] {
        return inFlight_ < maxInFlight_ || workerError_ != nullptr || stopRequested_;
    });
    rethrowWorkerErrorLocked();
    if (stopRequested_) {
        throw fq::error::ConfigurationError("gzip block writer is stopping");
    }
    workQueue_.push_back(Task{nextSubmit_++, std::move(block)});
    ++inFlight_;
    workCv_.notify_one();
}

void GzipBlockWriter::finishAll() {
    {
        std::unique_lock lock(mutex_);
        resultCv_.wait(lock,
                       [this] { return nextWrite_ >= nextSubmit_ || workerError_ != nullptr; });
        rethrowWorkerErrorLocked();
    }
    stopAndJoin();
    // writer 线程可能在最后一块写入时失败；join 后再检查一次。
    std::lock_guard lock(mutex_);
    rethrowWorkerErrorLocked();
}

auto GzipBlockWriter::submittedBlocks() -> bool {
    std::lock_guard lock(mutex_);
    return nextSubmit_ > 0;
}

auto GzipBlockWriter::compressBlock(const std::vector<char>& input, int level)
    -> std::vector<char> {
    if (input.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        throw fq::error::ConfigurationError("gzip block exceeds zlib uInt limit");
    }
    z_stream stream{};
    const int initResult = deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (initResult != Z_OK) {
        throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                        fq::error::ErrorSeverity::Critical,
                                        std::string("gzip block deflateInit2 failed: ") +
                                            zError(initResult));
    }
    struct DeflateGuard {
        z_stream* stream;
        ~DeflateGuard() {
            deflateEnd(stream);
        }
    } guard{&stream};

    const uLong bound = deflateBound(&stream, static_cast<uLong>(input.size()));
    std::vector<char> output(static_cast<size_t>(bound) + 64);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int result = deflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        const char* message = stream.msg != nullptr ? stream.msg : zError(result);
        throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                        fq::error::ErrorSeverity::Critical,
                                        std::string("gzip block deflate failed: ") + message);
    }
    output.resize(static_cast<size_t>(stream.total_out));
    return output;
}

void GzipBlockWriter::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            workCv_.wait(lock, [this] { return stopRequested_ || !workQueue_.empty(); });
            if (workQueue_.empty()) {
                if (stopRequested_) {
                    return;
                }
                continue;
            }
            task = std::move(workQueue_.front());
            workQueue_.pop_front();
        }

        try {
            auto compressed = compressBlock(task.input, compressionLevel_);
            std::lock_guard lock(mutex_);
            ready_.emplace(task.sequence, std::move(compressed));
            // resultCv_ 同时服务 writer 线程与 finishAll；notify_one 可能唤醒
            // finishAll 而让 writer 继续睡，造成丢唤醒死锁。
            resultCv_.notify_all();
        } catch (...) {
            std::lock_guard lock(mutex_);
            workerError_ = std::current_exception();
            stopRequested_ = true;
            workCv_.notify_all();
            spaceCv_.notify_all();
            resultCv_.notify_all();
            return;
        }
    }
}

void GzipBlockWriter::writerLoop() {
    for (;;) {
        std::vector<char> compressed;
        {
            std::unique_lock lock(mutex_);
            resultCv_.wait(lock, [this] {
                return ready_.find(nextWrite_) != ready_.end() || workerError_ != nullptr ||
                    (stopRequested_ && nextWrite_ >= nextSubmit_);
            });
            if (workerError_ != nullptr) {
                return;
            }
            auto it = ready_.find(nextWrite_);
            if (it == ready_.end()) {
                if (stopRequested_ && nextWrite_ >= nextSubmit_) {
                    return;
                }
                continue;
            }
            compressed = std::move(it->second);
            ready_.erase(it);
            ++nextWrite_;
            --inFlight_;
            spaceCv_.notify_one();
        }

        try {
            writeCompressed(compressed);
        } catch (...) {
            std::lock_guard lock(mutex_);
            workerError_ = std::current_exception();
            stopRequested_ = true;
            workCv_.notify_all();
            spaceCv_.notify_all();
            resultCv_.notify_all();
            return;
        }
        resultCv_.notify_all();
    }
}

void GzipBlockWriter::rethrowWorkerErrorLocked() {
    if (workerError_ != nullptr) {
        std::rethrow_exception(workerError_);
    }
}

void GzipBlockWriter::writeCompressed(const std::vector<char>& compressed) {
    const char* data = compressed.data();
    size_t remaining = compressed.size();
    off_t offset = flushedOffset_;
    while (remaining > 0) {
        const ssize_t written = ::write(fd_, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw fq::error::IOError(path_, errno);
        }
        if (written == 0) {
            throw fq::error::IOError(path_, 0);
        }
        data += written;
        remaining -= static_cast<size_t>(written);
        offset += written;
    }
#ifdef __linux__
    ::posix_fadvise(
        fd_, flushedOffset_, static_cast<off_t>(compressed.size()), POSIX_FADV_DONTNEED);
#endif
    flushedOffset_ = offset;
}

void GzipBlockWriter::stopAndJoin() {
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return;
        }
        stopRequested_ = true;
    }
    workCv_.notify_all();
    spaceCv_.notify_all();
    resultCv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    stopped_ = true;
}

}  // namespace fq::io::detail
