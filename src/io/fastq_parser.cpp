#include "fastq_parser.h"

#include "fqtools/error/error.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

namespace fq::io::detail {

namespace {

auto findEol(const char* ptr, const char* end) -> const char* {
    return static_cast<const char*>(std::memchr(ptr, '\n', static_cast<std::size_t>(end - ptr)));
}

}  // namespace

auto parseFastqRecords(const char* data,
                       std::size_t size,
                       std::size_t maxRecords,
                       bool eof,
                       FastqBatch& batch) -> FastqParseResult {
    if (maxRecords == 0) {
        throw std::invalid_argument("parseFastqRecords maxRecords must be >= 1");
    }
    if (size == 0) {
        return {};
    }

    const char* end = data + size;
    const char* ptr = data;
    const char* lastValidPtr = ptr;
    std::size_t parsedRecords = 0;

    while (ptr < end && parsedRecords < maxRecords) {
        while (ptr < end && (*ptr == '\n' || *ptr == '\r')) {
            ++ptr;
        }
        if (ptr >= end) {
            break;
        }

        if (*ptr != '@') {
            // 记录之间出现非空、非 '@' 字节：格式错误，不能当作 EOF 静默截断
            throw fq::error::FormatError(
                fmt::format("Expected '@' at record start. Found '{}'", *ptr));
        }

        const char* line1End = findEol(ptr, end);
        if (line1End == nullptr) {
            if (eof) {
                throw fq::error::FormatError("Incomplete FASTQ record: missing header newline");
            }
            break;
        }

        const char* line2Start = line1End + 1;
        const char* line2End = findEol(line2Start, end);
        if (line2End == nullptr) {
            if (eof) {
                throw fq::error::FormatError("Incomplete FASTQ record: missing sequence line");
            }
            break;
        }

        const char* line3Start = line2End + 1;
        if (line3Start >= end) {
            if (eof) {
                throw fq::error::FormatError("Incomplete FASTQ record: missing plus line");
            }
            break;
        }
        if (*line3Start != '+') {
            throw fq::error::FormatError(
                fmt::format("Expected '+' at line 3. Found '{}'", *line3Start));
        }
        const char* line3End = findEol(line3Start, end);
        if (line3End == nullptr) {
            if (eof) {
                throw fq::error::FormatError("Incomplete FASTQ record: missing plus newline");
            }
            break;
        }

        const char* line4Start = line3End + 1;
        if (line4Start >= end) {
            if (eof) {
                throw fq::error::FormatError("Incomplete FASTQ record: missing quality line");
            }
            break;
        }
        const char* line4End = findEol(line4Start, end);
        if (line4End == nullptr) {
            if (eof) {
                line4End = end;
            } else {
                break;
            }
        }

        FastqRecord rec;

        const auto kIdLen = static_cast<std::size_t>(line1End - ptr);
        std::size_t idLen = kIdLen;
        if (idLen > 0 && ptr[idLen - 1] == '\r') {
            --idLen;
        }
        const std::string_view kFullIdLine(ptr + 1, idLen > 0 ? (idLen - 1) : 0);
        const std::size_t kSpacePos = kFullIdLine.find_first_of(" \t");
        if (kSpacePos != std::string_view::npos) {
            rec.id = kFullIdLine.substr(0, kSpacePos);
            rec.comment = kFullIdLine.substr(kSpacePos + 1);
        } else {
            rec.id = kFullIdLine;
        }

        const auto kSeqLen = static_cast<std::size_t>(line2End - line2Start);
        std::size_t seqLen = kSeqLen;
        if (seqLen > 0 && line2Start[seqLen - 1] == '\r') {
            --seqLen;
        }
        rec.seq = std::string_view(line2Start, seqLen);

        const auto kPlusLen = static_cast<std::size_t>(line3End - line3Start);
        std::size_t plusLen = kPlusLen;
        if (plusLen > 0 && line3Start[plusLen - 1] == '\r') {
            --plusLen;
        }
        rec.plus = std::string_view(line3Start, plusLen);

        const auto kQualLen = static_cast<std::size_t>(line4End - line4Start);
        std::size_t qualLen = kQualLen;
        if (qualLen > 0 && line4Start[qualLen - 1] == '\r') {
            --qualLen;
        }
        rec.qual = std::string_view(line4Start, qualLen);

        if (rec.seq.empty()) {
            throw fq::error::FormatError(fmt::format("Empty sequence for read '{}'", rec.id));
        }
        if (!rec.validateLengths()) {
            throw fq::error::FormatError(
                fmt::format("Sequence and quality length mismatch for read '{}': {} vs {}",
                            rec.id,
                            rec.seq.size(),
                            rec.qual.size()));
        }

        batch.records().push_back(rec);
        ++parsedRecords;

        ptr = line4End;
        if (ptr < end && *ptr == '\n') {
            ++ptr;
        }
        lastValidPtr = ptr;
    }

    return {static_cast<std::size_t>(lastValidPtr - data), parsedRecords};
}

}  // namespace fq::io::detail
