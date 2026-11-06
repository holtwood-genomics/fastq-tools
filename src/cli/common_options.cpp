/**
 * @file common_options.cpp
 * @brief CLI 共享参数实现
 *
 */

#include "common_options.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "enum_parser.h"
#include <cxxopts.hpp>

#include "io/path_alias.h"
#include <fqtools/error/error.h>

namespace fq::cli {

namespace {

constexpr size_t kAutoThreadCap = 8;
constexpr size_t kAutoStdinThreadCap = 4;
constexpr std::uintmax_t kAutoParallelThresholdBytes = 64ULL * 1024ULL * 1024ULL;

/// CLI 默认并行度：避免在 32/64 核机器上默认创建过多 TBB token 与内存占用。
/// 小文件（<64 MiB）保留单线程：实测 49 MiB 输入下 auto 比 -t 1 慢约 12%，
/// 峰值 RSS 却高约 6 倍；大文件再启用并行。
/// 库 API 的 ProcessingOptions::threadCount 仍默认 1，保持显式控制。
auto defaultThreadCount(const std::string& inputPath) -> size_t {
    const auto hardware = std::thread::hardware_concurrency();
    const size_t cap =
        std::max<size_t>(1, std::min<size_t>(hardware == 0 ? 1 : hardware, kAutoThreadCap));
    if (inputPath.empty() || inputPath == "-") {
        return std::min<size_t>(cap, kAutoStdinThreadCap);
    }

    std::error_code error;
    const auto bytes = std::filesystem::file_size(inputPath, error);
    if (!error && bytes < kAutoParallelThresholdBytes) {
        return 1;
    }
    return cap;
}

auto parseThreadCount(const std::string& value, const std::string& inputPath) -> size_t {
    if (value == "auto") {
        return defaultThreadCount(inputPath);
    }

    if (value.empty() || value.front() == '-' || value.front() == '+') {
        throw std::invalid_argument("--threads must be 'auto' or a positive integer");
    }

    size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("--threads must be 'auto' or a positive integer");
    }
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument("--threads must be 'auto' or a positive integer");
    }
    return static_cast<size_t>(parsed);
}

}  // namespace

void CommonCliOptions::addOptions(cxxopts::Options& options) {
    options.add_options()("i,input", "Input FASTQ file", cxxopts::value<std::string>())(
        "o,output", "Output file", cxxopts::value<std::string>())(
        "t,threads",
        "Thread count: auto|N (auto = 1 for files <64 MiB, else min(hardware_concurrency, 8); "
        "stdin <=4; 1 = serial)",
        cxxopts::value<std::string>()->default_value("auto"))(
        "batch-size",
        "Batch size (reads per batch, default: 10000)",
        cxxopts::value<size_t>()->default_value("10000"))(
        "profile",
        "Performance profile: default|lowMemory|highThroughput",
        cxxopts::value<std::string>()->default_value("default"))(
        "memory-limit-gb",
        "Memory limit in GB (0=unlimited)",
        cxxopts::value<size_t>()->default_value("0"))(
        "batch-capacity-mb",
        "Buffer capacity per batch in MB; raise for very long reads, e.g. ONT "
        "(0=profile default: 4MB default / 1MB lowMemory / 16MB highThroughput)",
        cxxopts::value<size_t>()->default_value("0"))(
        "gzip-block-mb",
        "Gzip block-parallel output block size in MiB (0=serial single-member; "
        ">0=multi-member with --threads workers; filter command only)",
        cxxopts::value<size_t>()->default_value("0"));
}

auto CommonCliOptions::parse(const cxxopts::ParseResult& result) -> CommonCliOptions {
    CommonCliOptions opts;

    // 必需参数
    if (result.count("input")) {
        opts.inputPath = result["input"].as<std::string>();
    }
    if (result.count("output")) {
        opts.outputPath = result["output"].as<std::string>();
    }

    // 可选参数
    opts.threadCount = parseThreadCount(result["threads"].as<std::string>(), opts.inputPath);
    opts.batchSize = result["batch-size"].as<size_t>();

    // 解析性能预设
    std::string profileStr = result["profile"].as<std::string>();
    opts.profile = parseProcessingProfile(profileStr);

    // 内存限制
    size_t memGb = result["memory-limit-gb"].as<size_t>();
    if (memGb > 0) {
        opts.memoryLimitGb = memGb;
    }

    // 单批缓冲上限
    size_t batchCapacityMb = result["batch-capacity-mb"].as<size_t>();
    if (batchCapacityMb > 0) {
        opts.batchCapacityMb = batchCapacityMb;
    }

    // gzip 块级并行块大小（0 = 串行单流）
    size_t gzipBlockMb = result["gzip-block-mb"].as<size_t>();
    if (gzipBlockMb > 0) {
        opts.gzipBlockMb = gzipBlockMb;
    }

    return opts;
}

auto CommonCliOptions::toProcessingOptions() const -> processing::ProcessingOptions {
    // 单位换算上限：超出即物理上无意义，且乘 1024^n 会回绕 size_t
    constexpr size_t kMaxMemoryLimitGb = 1024ULL * 1024ULL;    // 1 PiB
    constexpr size_t kMaxBatchCapacityMb = 1024ULL * 1024ULL;  // 1 TiB
    constexpr size_t kMaxGzipBlockMb = 1024ULL;                // 1 GiB

    processing::ProcessingOptions opts;
    opts.batchSize = batchSize;
    opts.threadCount = threadCount;
    opts.profile = profile;

    if (memoryLimitGb.has_value()) {
        if (memoryLimitGb.value() > kMaxMemoryLimitGb) {
            throw std::invalid_argument("memory-limit-gb must be <= " +
                                        std::to_string(kMaxMemoryLimitGb));
        }
        opts.memoryLimitBytes = memoryLimitGb.value() * 1024ULL * 1024ULL * 1024ULL;
    }

    if (batchCapacityMb.has_value()) {
        if (batchCapacityMb.value() > kMaxBatchCapacityMb) {
            throw std::invalid_argument("batch-capacity-mb must be <= " +
                                        std::to_string(kMaxBatchCapacityMb));
        }
        opts.batchCapacityBytes = batchCapacityMb.value() * 1024ULL * 1024ULL;
    }

    if (gzipBlockMb.has_value()) {
        if (gzipBlockMb.value() > kMaxGzipBlockMb) {
            throw std::invalid_argument("gzip-block-mb must be <= " +
                                        std::to_string(kMaxGzipBlockMb));
        }
        opts.gzipBlockBytes = gzipBlockMb.value() * 1024ULL * 1024ULL;
    }

    return opts;
}

auto validateQualityEncoding(int qualityEncoding) -> int {
    if (qualityEncoding != 33 && qualityEncoding != 64) {
        throw std::invalid_argument("quality-encoding must be 33 or 64");
    }
    return qualityEncoding;
}

void validateReportTargets(const std::string& inputPath,
                           const std::string& outputPath,
                           const std::vector<std::string>& reportTargets) {
    // 报错消息用字符串拼接而非 fmt：fq_cli 不链接 fmt，保持依赖面最小
    for (const auto& target : reportTargets) {
        if (target.empty() || target == "-") {
            continue;
        }
        if (fq::io::pathsAlias(inputPath, target)) {
            // 用 += 逐段追加而非链式 +：后者每步都构造临时 string
            // （clang-tidy performance-inefficient-string-concatenation）
            std::string msg = "report target '";
            msg += target;
            msg += "' aliases the input FASTQ '";
            msg += inputPath;
            msg += "'; choose a different output path";
            throw fq::error::ConfigurationError(msg);
        }
        if (fq::io::pathsAlias(outputPath, target)) {
            std::string msg = "report target '";
            msg += target;
            msg += "' aliases the FASTQ output '";
            msg += outputPath;
            msg += "'; the report would overwrite the result file";
            throw fq::error::ConfigurationError(msg);
        }
    }
    for (size_t i = 0; i < reportTargets.size(); ++i) {
        for (size_t j = i + 1; j < reportTargets.size(); ++j) {
            if (fq::io::pathsAlias(reportTargets[i], reportTargets[j])) {
                throw fq::error::ConfigurationError("report targets '" + reportTargets[i] +
                                                    "' and '" + reportTargets[j] +
                                                    "' refer to the same file");
            }
        }
    }
}

}  // namespace fq::cli
