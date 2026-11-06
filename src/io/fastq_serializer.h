// FASTQ 纯序列化器。仅供 src 内部使用，不安装。
#pragma once

#include "fqtools/io/fastq_io.h"

#include <cstddef>
#include <span>
#include <vector>

namespace fq::io::detail {

/// @brief 一条记录序列化后的字节数（四行 + 换行 + 可选注释）
[[nodiscard]] auto serializedRecordSize(const FastqRecord& record) noexcept -> std::size_t;

/// @brief 将一条记录写入 dst，要求 dst 至少有 serializedRecordSize(record) 字节
/// @return 写入后的末指针
auto appendSerializedRecord(char* dst, const FastqRecord& record) noexcept -> char*;

/// @brief 将 records 的前缀追加到 out，尽量不超过 maxBytes
/// @details 即使 maxBytes 小于首条记录，也至少追加一条，保证调用方每次都能前进。
/// @return 追加的记录数
auto appendSerializedRecords(std::vector<char>& out,
                             std::span<const FastqRecord> records,
                             std::size_t maxBytes) -> std::size_t;

}  // namespace fq::io::detail
