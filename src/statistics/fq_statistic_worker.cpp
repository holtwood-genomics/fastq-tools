#include "statistics/fq_statistic_worker.h"

#include "fqtools/io/fastq_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fqtools/statistics/statistics_result.h>

namespace fq::statistics {

namespace {

/// 碱基字符 → 索引查找表（编译期构造，消除 switch/case 分支预测开销）
/// A/a=0, C/c=1, G/g=2, T/t=3, 其余=4(N)
constexpr auto buildBaseLut() -> std::array<uint8_t, 256> {
    std::array<uint8_t, 256> lut{};
    for (auto& v : lut) {
        v = 4;  // 默认为 N
    }
    lut[static_cast<unsigned char>('A')] = 0;
    lut[static_cast<unsigned char>('a')] = 0;
    lut[static_cast<unsigned char>('C')] = 1;
    lut[static_cast<unsigned char>('c')] = 1;
    lut[static_cast<unsigned char>('G')] = 2;
    lut[static_cast<unsigned char>('g')] = 2;
    lut[static_cast<unsigned char>('T')] = 3;
    lut[static_cast<unsigned char>('t')] = 3;
    return lut;
}

constexpr auto kBaseLut = buildBaseLut();

/// 头部 k-mer 草图表容量（与报告的 maxReportedSignatures 默认量级一致）。
constexpr size_t kHeadKmerSketchSize = 64;
/// 惰性最小堆的重建阈值：堆内允许堆积过期条目，超过后按当前槽位重建。
constexpr size_t kHeadKmerHeapRebuildThreshold = kHeadKmerSketchSize * 8;

/// 有界 Space-Saving 草图。
///
/// 旧实现每次遇到新 key 都在 `std::map` 上做一次 O(64) 的 `min_element` 并
/// erase/emplace 节点——每读一次都要付出这次扫描。这里改成固定槽位 +
/// 惰性最小堆：命中/插入只需 O(log 64) 的整数比较，槽位字符串就地复用，
/// 不再每次分配 map 节点。语义保持 Space-Saving 不变：满表时新 key 替换
/// 计数最小者并继承其计数 + 1。
class HeadKmerSketch {
public:
    /// 记录一次 key 出现；空 key 忽略（与旧实现的 `maxEntries == 0` 保护一致）。
    void add(std::string_view key) {
        if (key.empty()) {
            return;
        }

        if (const auto it = index_.find(key); it != index_.end()) {
            Slot& slot = slots_[it->second];
            ++slot.count;
            push({slot.count, it->second});
            return;
        }

        if (size_ < slots_.size()) {
            const auto idx = static_cast<uint16_t>(size_);
            Slot& slot = slots_[idx];
            slot.key.assign(key);
            slot.count = 1;
            index_.emplace(slot.key, idx);
            push({slot.count, idx});
            ++size_;
            return;
        }

        // 满表：先丢弃堆顶的过期条目（计数已增长或槽位已被替换）。
        while (!heap_.empty() && heap_.front().count != slots_[heap_.front().slot].count) {
            popMin();
        }
        if (heap_.empty()) {
            rebuildHeap();
        }
        if (heap_.empty()) {
            return;
        }

        const uint16_t victim = heap_.front().slot;
        popMin();
        const uint64_t inheritedCount = slots_[victim].count;
        index_.erase(slots_[victim].key);
        slots_[victim].key.assign(key);
        slots_[victim].count = inheritedCount + 1;
        index_.emplace(slots_[victim].key, victim);
        push({slots_[victim].count, victim});
    }

    /// 把草图内容写入聚合结果（每批只做一次）。
    void flushTo(std::map<std::string, uint64_t, std::less<>>& out) const {
        for (size_t i = 0; i < size_; ++i) {
            out.emplace(slots_[i].key, slots_[i].count);
        }
    }

private:
    struct Slot {
        std::string key;
        uint64_t count = 0;
    };

    struct HeapEntry {
        uint64_t count;
        uint16_t slot;
    };

    /// 最小堆：计数小者优先；计数相同时槽位号小者优先（确定性，不依赖 key 字符串比较）。
    struct MinHeap {
        auto operator()(const HeapEntry& lhs, const HeapEntry& rhs) const -> bool {
            if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
            }
            return lhs.slot > rhs.slot;
        }
    };

    struct TransparentHash {
        using is_transparent = void;
        auto operator()(std::string_view value) const noexcept -> size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    void push(HeapEntry entry) {
        heap_.push_back(entry);
        std::push_heap(heap_.begin(), heap_.end(), MinHeap{});
        if (heap_.size() > kHeadKmerHeapRebuildThreshold) {
            rebuildHeap();
        }
    }

    void popMin() {
        std::pop_heap(heap_.begin(), heap_.end(), MinHeap{});
        heap_.pop_back();
    }

    void rebuildHeap() {
        heap_.clear();
        heap_.reserve(kHeadKmerHeapRebuildThreshold + 1);
        for (size_t i = 0; i < size_; ++i) {
            heap_.push_back({slots_[i].count, static_cast<uint16_t>(i)});
        }
        std::make_heap(heap_.begin(), heap_.end(), MinHeap{});
    }

    std::array<Slot, kHeadKmerSketchSize> slots_{};
    size_t size_ = 0;
    std::unordered_map<std::string, uint16_t, TransparentHash, std::equal_to<>> index_;
    std::vector<HeapEntry> heap_;
};

}  // namespace

FqStatisticWorker::FqStatisticWorker(int qualOffset,
                                     size_t signatureKmerSize,
                                     size_t duplicateEstimateSampleModulo)
    : qualOffset_(qualOffset),
      signatureKmerSize_(std::max<size_t>(1, signatureKmerSize)),
      duplicateEstimateSampleModulo_(std::max<size_t>(1, duplicateEstimateSampleModulo)) {}

auto FqStatisticWorker::calculateStats(const Batch& batch) -> IStatistic::Result {
    FqStatisticResult result;
    if (batch.empty()) {
        return result;
    }

    HeadKmerSketch headKmers;

    for (const auto& read : batch) {
        result.readCount++;
        const size_t len = read.seq.size();
        result.totalBases += len;
        const size_t hashValue = std::hash<std::string_view>{}(read.seq);
        if ((hashValue % duplicateEstimateSampleModulo_) == 0) {
            auto& sampleCount = result.sampledSequenceHashes[hashValue];
            if (sampleCount > 0) {
                ++result.duplicateSampledReads;
            }
            ++sampleCount;
        }

        // 零拷贝取头部 k-mer：string_view::substr 会自动截断到实际长度。
        if (len > 0) {
            headKmers.add(std::string_view(read.seq).substr(0, signatureKmerSize_));
        }

        // 确保扁平化数组容量足够（仅在遇到更长 read 时 resize）
        result.ensureCapacity(len);

        const char* seqPtr = read.seq.data();

        // 碱基统计（查找表，无分支）；逐位置分布只记录前 kMaxPositionStats 个位置，
        // 全局总数始终完整累计。
        const size_t trackedLen = std::min(len, result.positionCount());
        for (size_t i = 0; i < len; ++i) {
            const size_t base = kBaseLut[static_cast<unsigned char>(seqPtr[i])];
            ++result.baseTotals[base];
            if (i < trackedLen) {
                result.baseAt(i)[base]++;
            }
        }

        // 质量分数统计（clamp 到 [0, kMaxQual)）。
        // 上界取 min(seq, qual)：内置 reader 保证两者等长，但 calculateStats 是公共接口，
        // 自定义 IStatistic/IReader 可注入不等长记录，防御性限界避免堆越界读。
        const size_t qualLen = std::min(len, read.qual.size());
        for (size_t i = 0; i < qualLen; ++i) {
            // char 符号性平台差异：显式按 int8_t 解释，使非法字节（>=128）在
            // x86/ARM 上一致视为负质量（与 AVX2 有符号比较语义一致）
            int qVal = static_cast<int>(static_cast<std::int8_t>(read.qual[i])) - qualOffset_;
            if (qVal < 0) {
                qVal = 0;
            }
            if (qVal >= kMaxQual) {
                qVal = kMaxQual - 1;
            }
            ++result.qualTotals[static_cast<size_t>(qVal)];
            if (i < trackedLen) {
                result.qualityAt(i)[qVal]++;
            }
        }
    }

    headKmers.flushTo(result.headKmerCounts);

    return result;
}

}  // namespace fq::statistics
