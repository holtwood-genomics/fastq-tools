// 文件目标类型判定。runtime/statistics 共用，仅供 src 内部使用，不安装。
#pragma once

#include <string>

#include <sys/stat.h>

namespace fq::io {

/// @brief 目标是否为特殊文件（字符/块设备、FIFO、socket）
/// @details 这些目标不支持"同目录临时文件 + rename"的原子发布协议：
///          /dev 下创建临时文件会被拒绝，且对设备 rename 覆盖也无意义。
///          目标不存在或 stat 失败时按普通文件处理。
[[nodiscard]] inline auto isSpecialFileTarget(const std::string& path) -> bool {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode) ||
        S_ISSOCK(st.st_mode);
}

}  // namespace fq::io
