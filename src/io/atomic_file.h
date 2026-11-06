// 临时文件发布协议。FastqWriter 与统计报告共用，仅供 src 内部使用，不安装。
#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace fq::io::detail {

/// @brief 已创建但尚未发布的临时文件
struct TemporaryFile {
    std::filesystem::path path;
    int fd = -1;
};

/// @brief 在目标同目录创建唯一临时文件（O_EXCL，失败时重试 EEXIST）
/// @throws fq::error::IOError 无法创建
[[nodiscard]] auto createTemporaryFile(const std::filesystem::path& target) -> TemporaryFile;

/// @brief 将临时文件原子 rename 到目标，并 best-effort fsync 父目录
/// @throws fq::error::IOError rename 失败
void publishTemporaryFile(const std::filesystem::path& temporaryPath,
                          const std::filesystem::path& target);

/// @brief 删除临时文件；失败时忽略，供异常清理路径使用
void removeTemporaryFile(const std::filesystem::path& temporaryPath) noexcept;

/// @brief 写入同目录临时文件，成功后原子发布到目标
/// @details writeBody 抛异常、流写失败、fsync/close 或 rename 失败时清理临时文件，
///          并保留原目标内容。写入失败抛 IOError。
void writeAtomically(const std::filesystem::path& target,
                     const std::function<void(std::ostream&)>& writeBody);

}  // namespace fq::io::detail
