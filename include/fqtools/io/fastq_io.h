/**
 * @file fastq_io.h
 * @brief FASTQ I/O 核心数据结构：零拷贝记录视图与批量容器
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fq::io {

/**
 * @brief 省略值初始化的分配器，仅用于「先 resize、后整块覆盖」的字节缓冲
 * @details std::vector::resize(n) 会值初始化新增元素，对 char 而言就是整块
 *          memset；读取路径上 resize(n) 之后紧接着 read/memcpy 覆盖每一个字节，
 *          这次清零纯属浪费。此分配器把默认构造路径变为空操作（char 这类
 *          隐式生存期类型由取得存储即创建对象，见 [basic.life]），带参构造
 *          仍执行真实构造，因此 assign/insert/push_back 的复制语义不变。
 * @note 只适用于 trivial 隐式生存期类型。resize 之后、写完之前的字节值不确定，
 *       禁止在该区间读取；任何依赖「新增元素为 0」的调用方不适用。
 */
template <class T>
struct NoInitAllocator {
    using value_type = T;

    NoInitAllocator() = default;
    template <class U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}

    auto allocate(std::size_t n) -> T* {
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T* p, std::size_t n) noexcept {
        std::allocator<T>{}.deallocate(p, n);
    }

    template <class U>
    void construct(U* /*p*/) noexcept {}
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <class U>
    auto operator==(const NoInitAllocator<U>&) const noexcept -> bool {
        return true;
    }
};

/// 读入 FASTQ 原始字节的缓冲：resize 不零初始化，由 I/O 整块覆盖
using ByteBuffer = std::vector<char, NoInitAllocator<char>>;

/**
 * @brief FASTQ 记录视图（零拷贝）
 * @details 所有字段均为 string_view，指向所属 FastqBatch 的连续内存。
 *          生命周期严格绑定到所属批次，不可逃逸。
 */
struct FastqRecord {
    std::string_view id;       ///< 序列 ID（不含 @ 前缀）
    std::string_view comment;  ///< ID 行的注释部分（可选）
    std::string_view seq;      ///< 序列内容（碱基）
    std::string_view qual;     ///< 质量值字符串
    std::string_view plus;     ///< '+' 行原文（含前导 +）

    [[nodiscard]] auto empty() const -> bool {
        return seq.empty();
    }

    [[nodiscard]] auto length() const -> size_t {
        return seq.size();
    }

    /// 校验序列与质量值长度是否匹配
    [[nodiscard]] auto validateLengths() const -> bool {
        return !seq.empty() && seq.size() == qual.size();
    }
};

/**
 * @brief FASTQ 批次数据容器（move-only）
 * @details 拥有连续内存，记录视图指向其内部偏移。
 *          clear() 复用内存不释放，适用于循环处理场景。
 *          禁止拷贝：records_ 中的视图指向 buffer_ 的堆存储，隐式拷贝会
 *          深拷贝 buffer_ 却保留指向源批次的视图（悬空读/串数据）。
 *          移动会转移 buffer_ 的存储，记录视图保持有效。
 */
class FastqBatch {
public:
    explicit FastqBatch(size_t capacityBytes = 4 * 1024 * 1024) {
        buffer_.reserve(capacityBytes);
        records_.reserve(capacityBytes / 150);
    }

    FastqBatch(size_t capacityBytes, size_t expectedRecords) {
        buffer_.reserve(capacityBytes);
        records_.reserve(expectedRecords);
    }

    FastqBatch(const FastqBatch&) = delete;
    auto operator=(const FastqBatch&) -> FastqBatch& = delete;
    FastqBatch(FastqBatch&&) noexcept = default;
    auto operator=(FastqBatch&&) noexcept -> FastqBatch& = default;

    void clear() {
        buffer_.clear();
        records_.clear();
    }

    /// @name 迭代器支持
    /// @{
    [[nodiscard]] auto begin() const {
        return records_.begin();
    }
    [[nodiscard]] auto end() const {
        return records_.end();
    }
    [[nodiscard]] auto begin() {
        return records_.begin();
    }
    [[nodiscard]] auto end() {
        return records_.end();
    }
    /// @}

    [[nodiscard]] auto size() const -> size_t {
        return records_.size();
    }

    [[nodiscard]] auto empty() const -> bool {
        return records_.empty();
    }

    /// @name 原始数据访问（供 Reader/Writer 使用）
    /// @{
    [[nodiscard]] auto buffer() -> ByteBuffer& {
        return buffer_;
    }
    [[nodiscard]] auto records() -> std::vector<FastqRecord>& {
        return records_;
    }
    [[nodiscard]] auto buffer() const -> const ByteBuffer& {
        return buffer_;
    }
    /// @}

private:
    ByteBuffer buffer_;
    std::vector<FastqRecord> records_;
};

}  // namespace fq::io
