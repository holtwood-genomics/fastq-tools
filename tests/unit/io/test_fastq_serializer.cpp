#include "fqtools/io/fastq_io.h"

#include <span>
#include <string>
#include <vector>

#include "io/fastq_serializer.h"
#include <gtest/gtest.h>

namespace fq::io::detail {

TEST(FastqSerializerTest, ComputesRecordSizeWithCommentAndDefaultPlus) {
    const FastqRecord plain{"read1", {}, "ACGT", "IIII", "+"};
    const FastqRecord withComment{"read2", "length=4", "AAAA", "JJJJ", "+"};
    const FastqRecord defaultPlus{"r3", {}, "TT", "II", {}};

    EXPECT_EQ(serializedRecordSize(plain), 19U);
    EXPECT_EQ(serializedRecordSize(withComment), 28U);
    EXPECT_EQ(serializedRecordSize(defaultPlus), 12U);
}

TEST(FastqSerializerTest, AppendRecordWritesCanonicalFastq) {
    const FastqRecord record{"read1", "comment", "ACGT", "IIII", "+read1"};
    const auto size = serializedRecordSize(record);
    std::vector<char> out(size);

    auto* end = appendSerializedRecord(out.data(), record);

    EXPECT_EQ(end, out.data() + size);
    EXPECT_EQ(std::string(out.begin(), out.end()), "@read1 comment\nACGT\n+read1\nIIII\n");
}

TEST(FastqSerializerTest, AppendRecordsRespectsByteBudgetAndAlwaysMakesProgress) {
    const std::vector<FastqRecord> records{
        {"r1", {}, "ACGT", "IIII", "+"},
        {"r2", "length=4", "AAAA", "JJJJ", "+"},
        {"r3", {}, "TT", "II", {}},
    };

    const auto firstSize = serializedRecordSize(records[0]);
    const auto secondSize = serializedRecordSize(records[1]);
    const auto thirdSize = serializedRecordSize(records[2]);

    std::vector<char> out;
    EXPECT_EQ(appendSerializedRecords(out, records, 0), 1U);
    EXPECT_EQ(std::string(out.begin(), out.end()), "@r1\nACGT\n+\nIIII\n");

    out.clear();
    EXPECT_EQ(appendSerializedRecords(out, records, firstSize + secondSize - 1), 1U);
    EXPECT_EQ(out.size(), firstSize);

    out.clear();
    EXPECT_EQ(appendSerializedRecords(out, records, firstSize + secondSize), 2U);
    EXPECT_EQ(out.size(), firstSize + secondSize);

    const auto third =
        appendSerializedRecords(out, std::span<const FastqRecord>(records).subspan(2), thirdSize);
    EXPECT_EQ(third, 1U);
    EXPECT_EQ(out.size(), firstSize + secondSize + thirdSize);
    EXPECT_EQ(std::string(out.end() - static_cast<std::ptrdiff_t>(thirdSize), out.end()),
              "@r3\nTT\n+\nII\n");
}

}  // namespace fq::io::detail
