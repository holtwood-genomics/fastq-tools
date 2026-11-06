/**
 * @file fastq_parser_core_fuzzer.cpp
 * @brief 纯 FASTQ 解析函数模糊测试
 *
 * 直接调用 fq::io::detail::parseFastqRecords，不经过文件系统、gzip 或
 * FastqReader 的 remainder 缓冲策略。覆盖 maxRecords 截断、!eof 残片、
 * eof 不完整记录、CRLF、前导空行与字段边界。
 *
 * 构建：cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ -B build-fuzz
 * 运行：./build-fuzz/fuzzers/fastq_parser_core_fuzzer tools/fuzz/corpus/ -max_len=4096
 */

#include "fqtools/error/error.h"
#include "fqtools/io/fastq_io.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "io/fastq_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1024 * 1024) {
        return 0;
    }

    FuzzedDataProvider dp(data, size);
    const auto maxRecords = dp.ConsumeIntegralInRange<size_t>(1, 64);
    const bool eof = dp.ConsumeBool();
    const auto payload = dp.ConsumeRemainingBytes<uint8_t>();
    std::vector<char> buffer(payload.begin(), payload.end());

    fq::io::FastqBatch batch(64 * 1024, maxRecords);
    try {
        const auto result = fq::io::detail::parseFastqRecords(
            buffer.data(), buffer.size(), maxRecords, eof, batch);

        if (result.recordCount > maxRecords || result.consumedBytes > buffer.size() ||
            batch.size() != result.recordCount) {
            __builtin_trap();
        }

        for (const auto& record : batch) {
            if (record.seq.empty() || record.seq.size() != record.qual.size()) {
                __builtin_trap();
            }
            volatile auto idSize = record.id.size();
            volatile auto commentSize = record.comment.size();
            volatile auto seqSize = record.seq.size();
            volatile auto qualSize = record.qual.size();
            volatile auto plusSize = record.plus.size();
            (void)idSize;
            (void)commentSize;
            (void)seqSize;
            (void)qualSize;
            (void)plusSize;
        }
    } catch (const fq::error::FastQException&) {
        // 畸形输入抛类型化异常是预期行为
    }

    return 0;
}
