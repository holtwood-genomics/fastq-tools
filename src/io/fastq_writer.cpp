#include "fqtools/io/fastq_writer.h"

#include "fqtools/error/error.h"
#include "fqtools/logging.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <sys/stat.h>

namespace fq::io {

namespace {

struct TemporaryFile {
    std::filesystem::path path;
    int fd = -1;
};

// 创建临时文件并返回持有其 fd 的结果：后续 gz/plain 写入直接复用该 fd，
// 消除"O_EXCL 创建后关闭、再以普通 open 重开"之间的 TOCTOU 窗口。
auto makeTemporaryFile(const std::filesystem::path& target) -> TemporaryFile {
    static std::atomic<std::uint64_t> counter{0};
    const auto parent =
        target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    const auto filename = target.filename().string();
    const auto processId = static_cast<unsigned long long>(::getpid());
    for (;;) {
        const auto suffix = ".tmp-" + std::to_string(processId) + "-" +
            std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        const auto candidate = parent / (filename + suffix);
        // 0666 交由 umask 过滤（惯例 0644）：rename 发布保留该权限，
        // 固定 0600 会无视调用方 umask，产出比预期更严格的权限
        const int descriptor = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (descriptor >= 0) {
            return {candidate, descriptor};
        }
        if (errno != EEXIST) {
            throw fq::error::IOError(candidate.string(), errno);
        }
    }
}

/// 多 gzip member 块级并行写出器。
/// 每个块独立 deflate 成完整 gzip member，按提交顺序拼接；解压结果与单流一致。
class GzipBlockWriter final {
public:
    GzipBlockWriter(int fd, std::string path, size_t threadCount, int compressionLevel)
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

    ~GzipBlockWriter() {
        stopAndJoin();
    }

    GzipBlockWriter(const GzipBlockWriter&) = delete;
    GzipBlockWriter& operator=(const GzipBlockWriter&) = delete;

    void submit(std::vector<char> block) {
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

    void finishAll() {
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

    [[nodiscard]] auto submittedBlocks() -> bool {
        std::lock_guard lock(mutex_);
        return nextSubmit_ > 0;
    }

private:
    struct Task {
        std::uint64_t sequence = 0;
        std::vector<char> input;
    };

    static auto compressBlock(const std::vector<char>& input, int level) -> std::vector<char> {
        if (input.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
            throw fq::error::ConfigurationError("gzip block exceeds zlib uInt limit");
        }
        z_stream stream{};
        const int initResult =
            deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
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

    void workerLoop() {
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

    void writerLoop() {
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

    void rethrowWorkerErrorLocked() {
        if (workerError_ != nullptr) {
            std::rethrow_exception(workerError_);
        }
    }

    void writeCompressed(const std::vector<char>& compressed) {
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

    void stopAndJoin() {
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

void validateGzipBlockOptions(const FastqWriterOptions& options) {
    constexpr size_t kMinBlockBytes = 1024;
    constexpr size_t kMaxBlockBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
    if (options.gzipBlockBytes < kMinBlockBytes || options.gzipBlockBytes > kMaxBlockBytes) {
        throw fq::error::ConfigurationError("gzip block size must be between 1 KiB and 1 GiB");
    }
    if (options.gzipThreads == 0 || options.gzipThreads > 64) {
        throw fq::error::ConfigurationError("gzip block worker count must be between 1 and 64");
    }
}

}  // namespace

static auto endsWithGzSuffix(const std::string& path) -> bool {
    constexpr const char* kGz = ".gz";
    if (path.size() < 3) {
        return false;
    }
    return path.compare(path.size() - 3, 3, kGz) == 0;
}

// 目标是否为特殊文件（字符/块设备、FIFO、socket）：
// 这些目标不支持"同目录临时文件 + rename"的原子发布协议——
// 在 /dev 下创建临时文件会被拒绝（Permission denied），
// 且对设备做 rename 覆盖也无意义。此类目标直接写入（与 stdout 语义一致）。
// 目标不存在或 stat 失败时按普通文件处理（走原子路径，open 阶段报错）。
auto isSpecialFileTarget(const std::string& path) -> bool {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode) ||
        S_ISSOCK(st.st_mode);
}

struct FastqWriter::Impl {
    int fd = -1;
    gzFile gzfile = nullptr;
    std::unique_ptr<GzipBlockWriter> blockWriter;
    bool ownsFd = true;
    bool streamingStdout = false;
    bool directWrite = false;  // 特殊文件目标（/dev/null 等）：无临时文件 + rename
    std::string path;
    std::filesystem::path temporaryPath;
    FastqWriterOptions options{};
    FastqWriterCompressionMode compression = FastqWriterCompressionMode::Auto;
    std::vector<char> buffer;

    // writer 只在执行后端的串行输出阶段被访问（IWriter 契约），无需原子计数
    std::uint64_t totalUncompressedBytes = 0;
    bool finished = false;

    // 记录已刷写到磁盘的文件偏移，用于 posix_fadvise DONTNEED
    off_t flushedOffset = 0;

    // gzdopen 会接管临时文件 fd 并在 gzclose 时关闭；dup 一份仅用于发布前 fsync，
    // 使 rename 前的数据页真正落盘（原子发布在掉电场景下的持久性保证）
    int syncFd = -1;

    explicit Impl(std::string p, const FastqWriterOptions& opt) : path(std::move(p)), options(opt) {
        if (path == "-") {
            openStdout();
            return;
        }

        if (isSpecialFileTarget(path)) {
            openDirect();
            return;
        }

        auto temp = makeTemporaryFile(path);
        temporaryPath = temp.path;

        try {
            if (options.compression == FastqWriterCompressionMode::Auto) {
                compression = endsWithGzSuffix(path) ? FastqWriterCompressionMode::Gzip
                                                     : FastqWriterCompressionMode::None;
            } else {
                compression = options.compression;
            }

            if (compression == FastqWriterCompressionMode::Gzip) {
                if (options.compressionLevel < 1 || options.compressionLevel > 9) {
                    throw fq::error::ConfigurationError(
                        "gzip compression level must be between 1 and 9");
                }
                if (options.gzipBlockBytes > 0) {
                    validateGzipBlockOptions(options);
                    fd = temp.fd;
                    temp.fd = -1;
                    blockWriter = std::make_unique<GzipBlockWriter>(
                        fd, temporaryPath.string(), options.gzipThreads, options.compressionLevel);
                } else {
                    // 使用 zlib gz API 写入 gzip 文件，默认压缩级别 6。
                    // gzdopen 接管临时文件 fd（gzclose 时一并关闭），不再重开文件。
                    const std::string mode = "wb" + std::to_string(options.compressionLevel);
                    // dup 供发布前 fsync（fd 所有权随后移交 gzFile，gzclose 会关闭原 fd）
                    syncFd = ::dup(temp.fd);
                    gzfile = gzdopen(temp.fd, mode.c_str());
                    if (!gzfile) {
                        throw fq::error::IOError(temporaryPath.string(), errno);
                    }
                    temp.fd = -1;  // fd 所有权移交给 gzFile
                    gzbuffer(gzfile, static_cast<unsigned>(options.outputBufferBytes));
                }
            } else {
                if (options.gzipBlockBytes > 0) {
                    throw fq::error::ConfigurationError("gzipBlockBytes requires gzip compression");
                }
                fd = temp.fd;
                temp.fd = -1;
            }

            buffer.reserve(options.gzipBlockBytes > 0 ? options.gzipBlockBytes
                                                      : options.outputBufferBytes);
        } catch (...) {
            if (temp.fd >= 0) {
                ::close(temp.fd);
            }
            if (gzfile) {
                gzclose(gzfile);
                gzfile = nullptr;
            }
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
            throw;
        }
    }

    void openStdout() {
        if (options.gzipBlockBytes > 0) {
            throw fq::error::ConfigurationError("gzip block output is not supported for stdout");
        }
        streamingStdout = true;
        ownsFd = false;
        fd = STDOUT_FILENO;
        compression = FastqWriterCompressionMode::None;
        buffer.reserve(options.outputBufferBytes);
    }

    // 特殊文件目标（字符/块设备、FIFO、socket）：绕过临时文件 + rename，
    // 直接以目标 fd 顺序写入；finish() 只 flush/关闭，不做发布。
    void openDirect() {
        directWrite = true;
        ownsFd = true;
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            throw fq::error::IOError(path, errno);
        }
        if (options.compression == FastqWriterCompressionMode::Auto) {
            compression = endsWithGzSuffix(path) ? FastqWriterCompressionMode::Gzip
                                                 : FastqWriterCompressionMode::None;
        } else {
            compression = options.compression;
        }
        if (compression == FastqWriterCompressionMode::Gzip) {
            if (options.compressionLevel < 1 || options.compressionLevel > 9) {
                ::close(fd);
                fd = -1;
                throw fq::error::ConfigurationError(
                    "gzip compression level must be between 1 and 9");
            }
            if (options.gzipBlockBytes > 0) {
                try {
                    validateGzipBlockOptions(options);
                    blockWriter = std::make_unique<GzipBlockWriter>(
                        fd, path, options.gzipThreads, options.compressionLevel);
                } catch (...) {
                    ::close(fd);
                    fd = -1;
                    throw;
                }
            } else {
                const std::string mode = "wb" + std::to_string(options.compressionLevel);
                gzfile = gzdopen(fd, mode.c_str());
                if (!gzfile) {
                    const int savedErrno = errno;
                    ::close(fd);
                    fd = -1;
                    throw fq::error::IOError(path, savedErrno);
                }
                fd = -1;  // fd 所有权移交给 gzFile
                gzbuffer(gzfile, static_cast<unsigned>(options.outputBufferBytes));
            }
        } else if (options.gzipBlockBytes > 0) {
            ::close(fd);
            fd = -1;
            throw fq::error::ConfigurationError("gzipBlockBytes requires gzip compression");
        }
        buffer.reserve(options.gzipBlockBytes > 0 ? options.gzipBlockBytes
                                                  : options.outputBufferBytes);
    }

    ~Impl() {
        // 析构只做兜底清理，绝不发布输出：异常展开到达这里时内容必然不完整，
        // 若调用 finish() 会把截断结果 rename 成目标文件，被误认为完整产物。
        // 发布只能经由显式 finish()（见 IWriter::finish 契约）。
        if (!finished) {
            cleanupTemporaryFile();
        }
    }

    void cleanupTemporaryFile() noexcept {
        // 先停 worker 并 join，再关闭 fd：避免压缩线程写入已关闭的 fd。
        blockWriter.reset();
        if (ownsFd && fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (gzfile) {
            gzclose(gzfile);
            gzfile = nullptr;
        }
        if (syncFd >= 0) {
            ::close(syncFd);
            syncFd = -1;
        }
        if (!temporaryPath.empty()) {
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
        }
    }

    void finish() {
        if (finished) {
            return;
        }

        try {
            flush();

            if (blockWriter) {
                // 空输入也要产出一个合法的空 gzip member，保持与串行 gzclose 一致。
                if (!blockWriter->submittedBlocks()) {
                    blockWriter->submit({});
                }
                blockWriter->finishAll();
            }

            if (streamingStdout) {
                finished = true;
                return;
            }

            if (directWrite) {
                // 特殊文件目标：无 rename 发布。设备/FIFO 无 page cache 语义，
                // 跳过 fsync；正确关闭即可（gzclose 会关闭其接管的 fd）。
                if (gzfile) {
                    const int closeResult = gzclose(gzfile);
                    gzfile = nullptr;
                    if (closeResult != Z_OK) {
                        throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                        fq::error::ErrorSeverity::Critical,
                                                        "gzip close failed");
                    }
                } else if (ownsFd && fd >= 0) {
                    if (::close(fd) != 0) {
                        const int error = errno;
                        fd = -1;
                        throw fq::error::IOError(path, error);
                    }
                    fd = -1;
                }
                finished = true;
                return;
            }

            if (ownsFd && fd >= 0) {
                // 数据落盘后再关闭：rename 发布前确保内容已到存储介质
                // （best-effort：fsync 失败不阻断发布，兼容 tmpfs 等无持久语义的挂载点）
                if (::fsync(fd) != 0) {
                    fq::logging::warn("fsync failed for '{}': {}", path, std::strerror(errno));
                }
                if (::close(fd) != 0) {
                    const int error = errno;
                    fd = -1;
                    throw fq::error::IOError(path, error);
                }
                fd = -1;
            }

            if (gzfile) {
                const int closeResult = gzclose(gzfile);
                gzfile = nullptr;
                if (closeResult != Z_OK) {
                    throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                    fq::error::ErrorSeverity::Critical,
                                                    "gzip close failed");
                }
                if (syncFd >= 0) {
                    if (::fsync(syncFd) != 0) {
                        fq::logging::warn("fsync failed for '{}': {}", path, std::strerror(errno));
                    }
                    ::close(syncFd);
                    syncFd = -1;
                }
            }

            std::error_code error;
            std::filesystem::rename(temporaryPath, path, error);
            if (error) {
                throw fq::error::IOError(path, error.value());
            }
            // rename 本身不保证持久：fsync 父目录使改名记录落盘
            // （best-effort，目录打开失败或 fsync 失败不阻断发布）
            const auto parent = std::filesystem::path(path).parent_path().empty()
                ? std::filesystem::path(".")
                : std::filesystem::path(path).parent_path();
            const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (dirFd >= 0) {
                ::fsync(dirFd);
                ::close(dirFd);
            }
            finished = true;
        } catch (...) {
            cleanupTemporaryFile();
            throw;
        }
    }

    void flush() {
        if (buffer.empty()) {
            return;
        }

        if (blockWriter) {
            std::vector<char> block;
            block.swap(buffer);
            buffer.reserve(options.gzipBlockBytes);
            blockWriter->submit(std::move(block));
            return;
        }

        if (compression == FastqWriterCompressionMode::Gzip) {
            // gzwrite 接受 unsigned int 参数：分块写出避免大缓冲静默截断。
            // 返回写入的未压缩字节数（0 表示错误）
            constexpr size_t kMaxGzChunk =
                static_cast<size_t>(std::numeric_limits<unsigned>::max()) / 2;
            const char* outPtr = buffer.data();
            size_t remaining = buffer.size();
            while (remaining > 0) {
                const auto toWrite = static_cast<unsigned>(std::min(remaining, kMaxGzChunk));
                const int written = gzwrite(gzfile, outPtr, toWrite);
                if (written <= 0 || static_cast<unsigned>(written) != toWrite) {
                    int err = 0;
                    const char* msg = gzerror(gzfile, &err);
                    throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                    fq::error::ErrorSeverity::Critical,
                                                    msg ? msg : "gzwrite failed");
                }
                outPtr += written;
                remaining -= static_cast<size_t>(written);
            }
        } else {
            const char* outPtr = buffer.data();
            size_t outSize = buffer.size();
            size_t totalWritten = 0;
            while (totalWritten < outSize) {
                const ssize_t written = ::write(fd, outPtr + totalWritten, outSize - totalWritten);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw fq::error::IOError(path, errno);
                }
                if (written == 0) {
                    throw fq::error::IOError(path, 0);
                }
                totalWritten += static_cast<size_t>(written);
            }

#ifdef __linux__
            if (!streamingStdout && !directWrite) {
                // 通知内核释放已写出的 page cache，减少大文件写出时的内存压力
                ::posix_fadvise(
                    fd, flushedOffset, static_cast<off_t>(outSize), POSIX_FADV_DONTNEED);
                flushedOffset += static_cast<off_t>(outSize);
            }
#endif
        }

        buffer.clear();
    }

    static auto recordSize(const FastqRecord& rec) -> size_t {
        const std::string_view plusLine = rec.plus.empty() ? std::string_view("+") : rec.plus;
        size_t needed = 1 + rec.id.size() + 1 +  // @ + ID + \n
            rec.seq.size() + 1 +                 // Seq + \n
            plusLine.size() + 1 +                // Plus + \n
            rec.qual.size() + 1;                 // Qual + \n

        if (!rec.comment.empty()) {
            needed += 1 + rec.comment.size();  // Space + Comment
        }
        return needed;
    }

    static void appendView(char*& dst, std::string_view value) {
        if (!value.empty()) {
            std::memcpy(dst, value.data(), value.size());
            dst += value.size();
        }
    }

    static void appendRecordBytes(char*& dst, const FastqRecord& rec) {
        const std::string_view plusLine = rec.plus.empty() ? std::string_view("+") : rec.plus;

        *dst++ = '@';
        appendView(dst, rec.id);

        if (!rec.comment.empty()) {
            *dst++ = ' ';
            appendView(dst, rec.comment);
        }
        *dst++ = '\n';

        appendView(dst, rec.seq);
        *dst++ = '\n';

        appendView(dst, plusLine);
        *dst++ = '\n';

        appendView(dst, rec.qual);
        *dst++ = '\n';
    }

    void ensureCapacity(size_t needed) {
        if (needed <= buffer.capacity() - buffer.size()) {
            return;
        }

        flush();
        if (needed <= buffer.capacity()) {
            return;
        }

        const size_t newCapacity = std::max(buffer.capacity() * 2, needed + 4096);
        buffer.reserve(newCapacity);
    }

    void appendRecord(const FastqRecord& rec) {
        const auto needed = recordSize(rec);
        ensureCapacity(needed);

        const auto oldSize = buffer.size();
        buffer.resize(oldSize + needed);
        auto* dst = buffer.data() + oldSize;
        appendRecordBytes(dst, rec);

        // 记录确实进入缓冲区后才计数：flush 抛异常时不把未接受的字节算进提交量
        totalUncompressedBytes += needed;
    }

    void appendBatch(const FastqBatch& batch) {
        auto first = batch.begin();
        while (first != batch.end()) {
            ensureCapacity(recordSize(*first));

            const auto available = buffer.capacity() - buffer.size();
            size_t totalSize = 0;
            auto next = first;
            while (next != batch.end()) {
                const auto needed = recordSize(*next);
                if (needed > available - totalSize) {
                    break;
                }
                totalSize += needed;
                ++next;
            }

            const auto oldSize = buffer.size();
            buffer.resize(oldSize + totalSize);
            auto* dst = buffer.data() + oldSize;
            for (auto record = first; record != next; ++record) {
                appendRecordBytes(dst, *record);
            }

            totalUncompressedBytes += totalSize;
            first = next;
        }
    }
};

FastqWriter::FastqWriter(const std::string& path) : FastqWriter(path, FastqWriterOptions{}) {}

FastqWriter::FastqWriter(const std::string& path, const FastqWriterOptions& options)
    : impl_(std::make_unique<Impl>(path, options)) {
    // Constructor logic verified in Impl
}

FastqWriter::~FastqWriter() = default;

FastqWriter::FastqWriter(FastqWriter&&) noexcept = default;
FastqWriter& FastqWriter::operator=(FastqWriter&&) noexcept = default;

bool FastqWriter::isOpen() const {
    return impl_ && (impl_->fd >= 0 || impl_->gzfile != nullptr);
}

auto FastqWriter::write(const FastqBatch& batch) -> std::uint64_t {
    if (!impl_ || impl_->finished) {
        throw fq::error::ConfigurationError("FastqWriter cannot write after finish");
    }
    const auto before = totalUncompressedBytes();
    impl_->appendBatch(batch);
    return totalUncompressedBytes() - before;
}

void FastqWriter::write(const FastqRecord& record) {
    if (!impl_ || impl_->finished) {
        throw fq::error::ConfigurationError("FastqWriter cannot write after finish");
    }
    impl_->appendRecord(record);
}

void FastqWriter::finish() {
    if (!impl_) {
        throw fq::error::ConfigurationError("FastqWriter is not initialized");
    }
    impl_->finish();
}

auto FastqWriter::totalUncompressedBytes() const -> std::uint64_t {
    return impl_ ? impl_->totalUncompressedBytes : 0;
}

}  // namespace fq::io
