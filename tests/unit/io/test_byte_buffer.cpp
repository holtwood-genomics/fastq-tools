/**
 * @file test_byte_buffer.cpp
 * @brief NoInitAllocator / ByteBuffer 的契约测试
 */

#include "fqtools/io/fastq_io.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace fq::io {

// resize 的默认构造路径不做零填充：已写过的字节应保持原值（读取路径依赖
// "resize 之后立即整块覆盖"，这里只验证没有额外 memset 覆盖旧数据）。
TEST(NoInitAllocatorTest, ResizeDoesNotZeroInitialize) {
    ByteBuffer buffer;
    buffer.resize(1024);
    std::memset(buffer.data(), 0xAA, buffer.size());

    buffer.clear();
    buffer.resize(1024);

    EXPECT_EQ(static_cast<unsigned char>(buffer.data()[0]), 0xAA);
    EXPECT_EQ(static_cast<unsigned char>(buffer.data()[1023]), 0xAA);
}

// 带参构造路径不受影响：assign/insert 仍然真实复制数据。
TEST(NoInitAllocatorTest, AssignAndInsertStillCopy) {
    ByteBuffer buffer;

    buffer.assign(4, 'x');
    EXPECT_EQ(std::string(buffer.data(), buffer.size()), "xxxx");

    buffer.insert(buffer.end(), {'y', 'z'});
    EXPECT_EQ(std::string(buffer.data(), buffer.size()), "xxxxyz");

    ByteBuffer filled(3, 'q');
    EXPECT_EQ(std::string(filled.data(), filled.size()), "qqq");
}

// FastqBatch 的缓冲复用容量，且 clear() 后重新 resize 不会丢弃已写入内容。
TEST(NoInitAllocatorTest, FastqBatchBufferReusesCapacity) {
    FastqBatch batch(8192);
    const auto capacityBefore = batch.buffer().capacity();

    batch.buffer().resize(64);
    std::memset(batch.buffer().data(), 'A', batch.buffer().size());

    batch.clear();
    batch.buffer().resize(64);

    EXPECT_GE(batch.buffer().capacity(), capacityBefore);
    EXPECT_EQ(batch.buffer().data()[63], 'A');
}

}  // namespace fq::io
