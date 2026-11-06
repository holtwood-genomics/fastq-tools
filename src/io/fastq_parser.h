// FASTQ 纯解析器。仅供 src 内部使用，不安装。
#pragma once

#include "fqtools/io/fastq_io.h"

#include <cstddef>

namespace fq::io::detail {

struct FastqParseResult {
    /// 最后一条完整记录之后的字节偏移（包含记录行与换行；不包含尾部残片）
    std::size_t consumedBytes = 0;
    /// 本次调用解析并追加到 batch.records() 的记录数
    std::size_t recordCount = 0;
};

/**
 * @brief 从连续缓冲区解析最多 maxRecords 条 FASTQ 记录
 * @param data 缓冲区起始地址
 * @param size 缓冲区字节数
 * @param maxRecords 本次最多解析记录数，必须 >= 1
 * @param eof 缓冲区之后是否已无更多输入
 * @param batch 记录追加到 batch.records()；字段 string_view 指向 data 指向的缓冲区
 * @return consumedBytes 为最后一条完整记录之后的偏移；recordCount 为本次解析条数
 * @throws fq::error::FormatError 格式错误；若 !eof，尾部不完整记录会保留为残片并返回上一条完整记录
 * @note 调用方负责保证 data 的生命周期覆盖 batch 中记录的使用期；本函数不复制记录内容。
 */
[[nodiscard]] auto parseFastqRecords(const char* data,
                                     std::size_t size,
                                     std::size_t maxRecords,
                                     bool eof,
                                     FastqBatch& batch) -> FastqParseResult;

}  // namespace fq::io::detail
