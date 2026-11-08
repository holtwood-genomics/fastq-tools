/**
 * @file statistics_result.h
 * @brief FASTQ 统计结果结构与合并语义
 */

#pragma once

#include <algorithm>
#include <array>
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
    /// 逐位置分布只保留前 N 个位置，避免超长 read 把内存放大到 GiB 级；
    /// 全局碱基/质量总数仍完整统计。
    static constexpr size_t kMaxPositionStats = 16384;

    uint64_t readCount = 0;              ///< 总读取数量
    uint64_t totalBases = 0;             ///< 总碱基数
    uint32_t maxReadLength = 0;          ///< 最大读取长度（真实值，不受位置上限影响）
    uint64_t duplicateSampledReads = 0;  ///< 采样命中的 duplicate reads 数量

    /// 全局碱基总数（A/C/G/T/N），覆盖超过位置上限的 read。
    std::array<uint64_t, kMaxBaseNum> baseTotals{};
    /// 全局质量分数直方图，覆盖超过位置上限的 read。
    std::array<uint64_t, kMaxQual> qualTotals{};

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
        maxReadLength = static_cast<std::uint32_t>(newLength);

        const size_t trackedLength = std::min(newLength, kMaxPositionStats);
        if (trackedLength <= positionCount()) {
            return;
        }
        if (trackedLength > posQualityDist.max_size() / static_cast<size_t>(kMaxQual) ||
            trackedLength > posBaseDist.max_size() / static_cast<size_t>(kMaxBaseNum)) {
            throw std::length_error(
                "FqStatisticResult position distribution exceeds container limit");
        }

        posQualityDist.resize(trackedLength * static_cast<size_t>(kMaxQual), 0);
        posBaseDist.resize(trackedLength * static_cast<size_t>(kMaxBaseNum), 0);
    }

    /// 实际保留逐位置分布的位置数（≤ kMaxPositionStats）
    [[nodiscard]] auto positionCount() const -> size_t {
        return posQualityDist.size() / static_cast<size_t>(kMaxQual);
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
