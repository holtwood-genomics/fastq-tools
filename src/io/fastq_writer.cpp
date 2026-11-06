#include "fqtools/io/fastq_writer.h"

#include "fqtools/error/error.h"
#include "fqtools/logging.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atomic_file.h"
#include "fastq_serializer.h"
#include "file_target.h"
#include "gzip_block_writer.h"
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

namespace fq::io {

namespace {

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

struct FastqWriter::Impl {
    int fd = -1;
    gzFile gzfile = nullptr;
    std::unique_ptr<detail::GzipBlockWriter> blockWriter;
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

        // 压缩模式与所有校验都在创建任何资源之前完成：
        // 非法选项不会留下临时文件、fd 或已启动的压缩线程。
        compression = resolveCompression();
        validateCompressionOptions();
        reserveOutputBuffer();

        if (isSpecialFileTarget(path)) {
            openDirect();
            return;
        }

        auto temp = detail::createTemporaryFile(path);
        temporaryPath = temp.path;

        try {
            if (compression == FastqWriterCompressionMode::Gzip) {
                if (options.gzipBlockBytes > 0) {
                    fd = temp.fd;
                    temp.fd = -1;
                    blockWriter = std::make_unique<detail::GzipBlockWriter>(
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
                fd = temp.fd;
                temp.fd = -1;
            }
        } catch (...) {
            // 先停压缩线程再关闭 fd；否则 worker 可能写入已关闭的 fd。
            blockWriter.reset();
            if (temp.fd >= 0) {
                ::close(temp.fd);
            }
            if (gzfile) {
                gzclose(gzfile);
                gzfile = nullptr;
            }
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            if (syncFd >= 0) {
                ::close(syncFd);
                syncFd = -1;
            }
            detail::removeTemporaryFile(temporaryPath);
            throw;
        }
    }

    [[nodiscard]] auto resolveCompression() const -> FastqWriterCompressionMode {
        if (options.compression != FastqWriterCompressionMode::Auto) {
            return options.compression;
        }
        return endsWithGzSuffix(path) ? FastqWriterCompressionMode::Gzip
                                      : FastqWriterCompressionMode::None;
    }

    void validateCompressionOptions() const {
        if (compression != FastqWriterCompressionMode::Gzip) {
            if (options.gzipBlockBytes > 0) {
                throw fq::error::ConfigurationError("gzipBlockBytes requires gzip compression");
            }
            return;
        }
        if (options.compressionLevel < 1 || options.compressionLevel > 9) {
            throw fq::error::ConfigurationError("gzip compression level must be between 1 and 9");
        }
        if (options.gzipBlockBytes > 0) {
            validateGzipBlockOptions(options);
        }
    }

    void reserveOutputBuffer() {
        buffer.reserve(options.gzipBlockBytes > 0 ? options.gzipBlockBytes
                                                  : options.outputBufferBytes);
    }

    void openStdout() {
        if (options.gzipBlockBytes > 0) {
            throw fq::error::ConfigurationError("gzip block output is not supported for stdout");
        }
        streamingStdout = true;
        ownsFd = false;
        fd = STDOUT_FILENO;
        compression = FastqWriterCompressionMode::None;
        reserveOutputBuffer();
    }

    // 特殊文件目标（字符/块设备、FIFO、socket）：绕过临时文件 + rename，
    // 直接以目标 fd 顺序写入；finish() 只 flush/关闭，不做发布。
    void openDirect() {
        directWrite = true;
        ownsFd = true;

        // 可能在 open 之后抛异常的操作先完成（字符串分配），避免持有 fd 再做可能失败的工作。
        std::string gzipMode;
        if (compression == FastqWriterCompressionMode::Gzip && options.gzipBlockBytes == 0) {
            gzipMode = "wb" + std::to_string(options.compressionLevel);
        }

        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT, 0666);
        if (descriptor < 0) {
            throw fq::error::IOError(path, errno);
        }

        if (compression != FastqWriterCompressionMode::Gzip) {
            fd = descriptor;
            return;
        }

        if (options.gzipBlockBytes > 0) {
            try {
                blockWriter = std::make_unique<detail::GzipBlockWriter>(
                    descriptor, path, options.gzipThreads, options.compressionLevel);
            } catch (...) {
                ::close(descriptor);
                throw;
            }
            fd = descriptor;
            return;
        }

        gzfile = gzdopen(descriptor, gzipMode.c_str());
        if (!gzfile) {
            const int savedErrno = errno;
            ::close(descriptor);
            throw fq::error::IOError(path, savedErrno);
        }
        fd = -1;  // fd 所有权移交给 gzFile
        gzbuffer(gzfile, static_cast<unsigned>(options.outputBufferBytes));
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
        detail::removeTemporaryFile(temporaryPath);
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

            detail::publishTemporaryFile(temporaryPath, path);
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
        const auto needed = detail::serializedRecordSize(rec);
        ensureCapacity(needed);

        const auto oldSize = buffer.size();
        buffer.resize(oldSize + needed);
        detail::appendSerializedRecord(buffer.data() + oldSize, rec);

        // 记录确实进入缓冲区后才计数：flush 抛异常时不把未接受的字节算进提交量
        totalUncompressedBytes += needed;
    }

    void appendBatch(const FastqBatch& batch) {
        const std::span<const FastqRecord> records(batch.begin(), batch.size());
        size_t first = 0;
        while (first < records.size()) {
            ensureCapacity(detail::serializedRecordSize(records[first]));

            const auto available = buffer.capacity() - buffer.size();
            const auto oldSize = buffer.size();
            first += detail::appendSerializedRecords(buffer, records.subspan(first), available);
            totalUncompressedBytes += buffer.size() - oldSize;
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
