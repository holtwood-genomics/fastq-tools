/**
 * @file statistics_result.h
 * @brief FASTQ 统计结果结构与合并语义
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fq::statistics {

/// 每个位置的质量分数槽位数
inline constexpr int kMaxQual = 42;
/// 每个位置的碱基槽位数（A/C/G/T/N）
inline constexpr int kMaxBaseNum = 5;

/**
 * @brief FASTQ 统计结果
 * @details 保存读取数量、最大读长、位置质量/碱基分布、头部 k-mer 和 duplicate 采样结果。
 *          位置分布使用扁平化一维布局：`pos * kMaxQual + qual` 与 `pos * kMaxBaseNum + base`。
 */
struct FqStatisticResult {
    uint64_t readCount = 0;              ///< 总读取数量
    uint64_t totalBases = 0;             ///< 总碱基数
    uint32_t maxReadLength = 0;          ///< 最大读取长度
    uint64_t duplicateSampledReads = 0;  ///< 采样命中的 duplicate reads 数量

    /// 位置质量分数分布（扁平化一维布局：[pos * kMaxQual + qual]）
    std::vector<uint64_t> posQualityDist;
    /// 位置碱基分布（扁平化一维布局：[pos * kMaxBaseNum + base]）
    std::vector<uint64_t> posBaseDist;
    std::map<std::string, uint64_t, std::less<>> headKmerCounts;  ///< 头部 k-mer signature 计数
    std::map<uint64_t, uint64_t> sampledSequenceHashes;           ///< duplicate 采样哈希计数

    /**
     * @brief 确保分布数组能容纳 newLength 个位置
     * @throws std::length_error newLength 超出 maxReadLength 的 uint32 范围，或
     *         扁平化数组长度超出容器上限
     */
    void ensureCapacity(size_t newLength) {
        if (newLength <= maxReadLength) {
            return;
        }

        constexpr auto kMaxTrackedLength =
            static_cast<size_t>(std::numeric_limits<std::uint32_t>::max());
        if (newLength > kMaxTrackedLength) {
            throw std::length_error("FqStatisticResult read length exceeds uint32 range");
        }
        if (newLength > posQualityDist.max_size() / static_cast<size_t>(kMaxQual) ||
            newLength > posBaseDist.max_size() / static_cast<size_t>(kMaxBaseNum)) {
            throw std::length_error(
                "FqStatisticResult position distribution exceeds container limit");
        }

        posQualityDist.resize(newLength * static_cast<size_t>(kMaxQual), 0);
        posBaseDist.resize(newLength * static_cast<size_t>(kMaxBaseNum), 0);
        maxReadLength = static_cast<std::uint32_t>(newLength);
    }

    /// 按位置访问质量分布（返回指向该位置 kMaxQual 个槽位的指针）
    [[nodiscard]] auto qualityAt(size_t pos) -> uint64_t* {
        return posQualityDist.data() + pos * kMaxQual;
    }
    [[nodiscard]] auto qualityAt(size_t pos) const -> const uint64_t* {
        return posQualityDist.data() + pos * kMaxQual;
    }

    /// 按位置访问碱基分布（返回指向该位置 kMaxBaseNum 个槽位的指针）
    [[nodiscard]] auto baseAt(size_t pos) -> uint64_t* {
        return posBaseDist.data() + pos * kMaxBaseNum;
    }
    [[nodiscard]] auto baseAt(size_t pos) const -> const uint64_t* {
        return posBaseDist.data() + pos * kMaxBaseNum;
    }

    /// 合并另一份统计结果（读取数、分布、signature 与 duplicate 采样）
    auto operator+=(const FqStatisticResult& other) -> FqStatisticResult&;
};

}  // namespace fq::statistics
