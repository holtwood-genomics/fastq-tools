#include "fastq_serializer.h"

#include <cstring>
#include <string_view>

namespace fq::io::detail {

namespace {

void appendView(char*& dst, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(dst, value.data(), value.size());
        dst += value.size();
    }
}

[[nodiscard]] auto resolvedPlusLine(const FastqRecord& record) noexcept -> std::string_view {
    return record.plus.empty() ? std::string_view("+") : record.plus;
}

}  // namespace

auto serializedRecordSize(const FastqRecord& record) noexcept -> std::size_t {
    const auto plusLine = resolvedPlusLine(record);
    std::size_t size = 1 + record.id.size() + 1 + record.seq.size() + 1 + plusLine.size() + 1 +
        record.qual.size() + 1;
    if (!record.comment.empty()) {
        size += 1 + record.comment.size();
    }
    return size;
}

auto appendSerializedRecord(char* dst, const FastqRecord& record) noexcept -> char* {
    *dst++ = '@';
    appendView(dst, record.id);

    if (!record.comment.empty()) {
        *dst++ = ' ';
        appendView(dst, record.comment);
    }
    *dst++ = '\n';

    appendView(dst, record.seq);
    *dst++ = '\n';

    appendView(dst, resolvedPlusLine(record));
    *dst++ = '\n';

    appendView(dst, record.qual);
    *dst++ = '\n';
    return dst;
}

auto appendSerializedRecords(std::vector<char>& out,
                             std::span<const FastqRecord> records,
                             std::size_t maxBytes) -> std::size_t {
    if (records.empty()) {
        return 0;
    }

    std::size_t bytes = 0;
    std::size_t count = 0;
    for (const auto& record : records) {
        const auto needed = serializedRecordSize(record);
        if (count > 0 && (bytes > maxBytes || needed > maxBytes - bytes)) {
            break;
        }
        bytes += needed;
        ++count;
    }

    const auto oldSize = out.size();
    out.resize(oldSize + bytes);
    auto* dst = out.data() + oldSize;
    for (std::size_t i = 0; i < count; ++i) {
        dst = appendSerializedRecord(dst, records[i]);
    }
    return count;
}

}  // namespace fq::io::detail
