#include "atomic_file.h"

#include "fqtools/error/error.h"
#include "fqtools/logging.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <streambuf>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace fq::io::detail {

namespace {

class FdStreamBuffer final : public std::streambuf {
public:
    explicit FdStreamBuffer(int fd) : fd_(fd) {
        setp(buffer_, buffer_ + sizeof(buffer_));
    }

    ~FdStreamBuffer() override {
        static_cast<void>(flushBuffer());
    }

    [[nodiscard]] auto error() const -> int {
        return error_;
    }

protected:
    auto overflow(int_type ch) -> int_type override {
        if (!flushBuffer()) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            const char value = traits_type::to_char_type(ch);
            if (!writeAll(&value, 1)) {
                return traits_type::eof();
            }
        }
        return traits_type::not_eof(ch);
    }

    auto xsputn(const char* data, std::streamsize size) -> std::streamsize override {
        if (size <= 0) {
            return 0;
        }

        size_t remaining = static_cast<size_t>(size);
        size_t written = 0;
        while (remaining > 0) {
            if (pptr() == epptr() && !flushBuffer()) {
                break;
            }
            const size_t space = static_cast<size_t>(epptr() - pptr());
            const size_t chunk = std::min(space, remaining);
            std::memcpy(pptr(), data + written, chunk);
            pbump(static_cast<int>(chunk));
            written += chunk;
            remaining -= chunk;
        }
        return static_cast<std::streamsize>(written);
    }

    auto sync() -> int override {
        return flushBuffer() ? 0 : -1;
    }

private:
    auto flushBuffer() -> bool {
        const size_t size = static_cast<size_t>(pptr() - pbase());
        if (size == 0) {
            return true;
        }
        if (!writeAll(pbase(), size)) {
            return false;
        }
        setp(buffer_, buffer_ + sizeof(buffer_));
        return true;
    }

    auto writeAll(const char* data, size_t size) -> bool {
        size_t written = 0;
        while (written < size) {
            const ssize_t result = ::write(fd_, data + written, size - written);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error_ = errno;
                return false;
            }
            if (result == 0) {
                error_ = EIO;
                return false;
            }
            written += static_cast<size_t>(result);
        }
        return true;
    }

    int fd_;
    int error_ = 0;
    char buffer_[64 * 1024];
};

void syncDirectory(const std::filesystem::path& parent) noexcept {
    const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirFd < 0) {
        return;
    }
    static_cast<void>(::fsync(dirFd));
    ::close(dirFd);
}

}  // namespace

auto createTemporaryFile(const std::filesystem::path& target) -> TemporaryFile {
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

void publishTemporaryFile(const std::filesystem::path& temporaryPath,
                          const std::filesystem::path& target) {
    const auto parent =
        target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    std::error_code error;
    std::filesystem::rename(temporaryPath, target, error);
    if (error) {
        throw fq::error::IOError(target.string(), error.value());
    }
    syncDirectory(parent);
}

void removeTemporaryFile(const std::filesystem::path& temporaryPath) noexcept {
    if (temporaryPath.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(temporaryPath, error);
}

void writeAtomically(const std::filesystem::path& target,
                     const std::function<void(std::ostream&)>& writeBody) {
    auto temporaryFile = createTemporaryFile(target);
    try {
        {
            FdStreamBuffer streamBuffer(temporaryFile.fd);
            std::ostream writer(&streamBuffer);
            writeBody(writer);
            writer.flush();
            if (!writer.good()) {
                throw fq::error::IOError(target.string(),
                                         streamBuffer.error() == 0 ? EIO : streamBuffer.error());
            }
        }

        // 与 FastqWriter 一致：fsync 失败不阻断发布，兼容 tmpfs 等无持久语义的挂载点。
        if (::fsync(temporaryFile.fd) != 0) {
            fq::logging::warn("fsync failed for '{}': {}", target.string(), std::strerror(errno));
        }
        if (::close(temporaryFile.fd) != 0) {
            const int error = errno;
            temporaryFile.fd = -1;
            throw fq::error::IOError(target.string(), error);
        }
        temporaryFile.fd = -1;

        publishTemporaryFile(temporaryFile.path, target);
    } catch (...) {
        if (temporaryFile.fd >= 0) {
            ::close(temporaryFile.fd);
        }
        removeTemporaryFile(temporaryFile.path);
        throw;
    }
}

}  // namespace fq::io::detail
