/**
 * @file concurrency_benchmark.cpp
 * @brief 真实生产流水线的线程数与批大小扫描
 * @details 与固定口径的生产基线分开，避免探索性矩阵改变既有快照或拖慢默认套件。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "benchmark_support.h"

#include <benchmark/benchmark.h>
#include <fqtools/processing/mutators/quality_trimmer.h>
#include <fqtools/processing/predicates.h>
#include <fqtools/processing/processing_pipeline_interface.h>
#include <fqtools/statistics/interfaces.h>

namespace fq::benchmark {

namespace {

auto makeProcessingOptions(
    std::size_t threadCount,
    std::size_t batchSize,
    fq::processing::ProcessingProfile profile = fq::processing::ProcessingProfile::Default)
    -> fq::processing::ProcessingOptions {
    fq::processing::ProcessingOptions options;
    options.threadCount = threadCount;
    options.batchSize = batchSize;
    options.profile = profile;
    return options;
}

void configureCombinedFilter(fq::processing::Pipeline& pipeline) {
    pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(25.0, 100));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(100));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinQualityPredicate>(25.0));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MaxNRatioPredicate>(0.1));
}

auto makeOutputPath(std::string_view kind,
                    std::size_t threadCount,
                    std::size_t batchSize,
                    std::string_view suffix) -> std::filesystem::path {
    return std::filesystem::temp_directory_path() /
        ("fastqtools-concurrency-" + std::string(kind) + "-" + std::to_string(threadCount) + "-" +
         std::to_string(batchSize) + std::string(suffix));
}

void setConcurrencyCounters(::benchmark::State& state,
                            std::size_t threadCount,
                            std::size_t batchSize) {
    state.counters["threads"] = static_cast<double>(threadCount);
    state.counters["batch_records"] = static_cast<double>(batchSize);
}

void benchmarkFilter(::benchmark::State& state,
                     bool writeOutput,
                     fq::processing::ProcessingProfile profile,
                     bool configureFilter,
                     bool gzipOutput) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto threadCount = static_cast<std::size_t>(state.range(0));
    const auto batchSize = static_cast<std::size_t>(state.range(1));
    const auto outputKind =
        writeOutput ? (gzipOutput ? "filter-gzip" : "filter-plain") : "filter-cpu";
    const auto outputPath =
        makeOutputPath(outputKind, threadCount, batchSize, gzipOutput ? ".fastq.gz" : ".fastq");

    std::uint64_t passedReads = 0;
    std::uint64_t filteredReads = 0;
    std::uint64_t modifiedReads = 0;
    const auto wallStart = std::chrono::steady_clock::now();
    for (auto _ : state) {
        fq::processing::Pipeline pipeline;
        pipeline.setInputPath(inputPath.string());
        if (writeOutput) {
            pipeline.setOutputPath(outputPath.string());
        }
        pipeline.setProcessingOptions(makeProcessingOptions(threadCount, batchSize, profile));
        if (configureFilter) {
            configureCombinedFilter(pipeline);
        }

        const auto stats = pipeline.run();
        passedReads = stats.passedReads;
        filteredReads = stats.filteredReads;
        modifiedReads = stats.modifiedReads;
        ::benchmark::DoNotOptimize(passedReads);
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    if (writeOutput) {
        removeBenchmarkOutput(outputPath);
    }

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    setConcurrencyCounters(state, threadCount, batchSize);
    state.counters["gzip_output"] = gzipOutput ? 1.0 : 0.0;
    state.counters["passed_reads"] = static_cast<double>(passedReads);
    state.counters["filtered_reads"] = static_cast<double>(filteredReads);
    state.counters["modified_reads"] = static_cast<double>(modifiedReads);
}

void benchmarkReadQc(::benchmark::State& state) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto threadCount = static_cast<std::size_t>(state.range(0));
    const auto batchSize = static_cast<std::size_t>(state.range(1));

    const auto wallStart = std::chrono::steady_clock::now();
    for (auto _ : state) {
        fq::statistics::StatisticOptions options;
        options.inputFastqPath = inputPath.string();
        options.processing = makeProcessingOptions(threadCount, batchSize);
        fq::statistics::Calculator calculator(std::move(options));
        calculator.run();
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    setConcurrencyCounters(state, threadCount, batchSize);
}

/// 多 member gzip 块级并行的 writer 矩阵：块大小 × worker 数。
/// 只测 writer，避免 pipeline 调度噪声；该矩阵属于探索性套件，默认不进入基线。
void benchmarkGzipBlockWriter(::benchmark::State& state) {
    const auto blockMiB = static_cast<std::size_t>(state.range(0));
    const auto threads = static_cast<std::size_t>(state.range(1));
    const auto blockBytes = blockMiB * 1024 * 1024;
    const auto& batches = loadBenchmarkBatches();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto outputPath = std::filesystem::temp_directory_path() /
        ("fastqtools-gzip-block-" + std::to_string(blockMiB) + "m-" + std::to_string(threads) +
         "t.fastq.gz");

    const auto wallStart = std::chrono::steady_clock::now();
    for (auto _ : state) {
        fq::io::FastqWriterOptions options;
        options.compression = fq::io::FastqWriterCompressionMode::Gzip;
        options.compressionLevel = 6;
        options.gzipBlockBytes = blockBytes;
        options.gzipThreads = threads;
        fq::io::FastqWriter writer(outputPath.string(), options);
        for (const auto& batch : batches) {
            writer.write(batch);
        }
        writer.finish();
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    removeBenchmarkOutput(outputPath);
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    state.counters["includes_publish"] = 1.0;
    state.counters["gzip_block_bytes"] = static_cast<double>(blockBytes);
    state.counters["gzip_threads"] = static_cast<double>(threads);
    state.counters["compression_level"] = 6.0;
}

}  // namespace

BENCHMARK_CAPTURE(
    benchmarkFilter, cpu, false, fq::processing::ProcessingProfile::Default, true, false)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(
    benchmarkFilter, cpu_no_filter, false, fq::processing::ProcessingProfile::Default, false, false)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(
    benchmarkFilter, plain_output, true, fq::processing::ProcessingProfile::Default, true, false)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkFilter,
                  plain_output_high_throughput,
                  true,
                  fq::processing::ProcessingProfile::HighThroughput,
                  true,
                  false)
    ->ArgsProduct({{1, 2, 4, 8}, {10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

// gzip 输出贯穿 reader → filter → writer 的完整生产路径；
// 1/2/4/8 线程的曲线用于判断压缩是否仍是串行瓶颈。
BENCHMARK_CAPTURE(
    benchmarkFilter, gzip_output, true, fq::processing::ProcessingProfile::Default, true, true)
    ->ArgsProduct({{1, 2, 4, 8}, {10'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK(benchmarkGzipBlockWriter)
    ->ArgsProduct({{1, 4, 16}, {1, 2, 4, 8}})
    ->Unit(::benchmark::kMillisecond);

// 探索性矩阵：runner 统一消费 harness 记录的 steady_clock wall time；
// 该组只用于比较并行墙钟吞吐，不进入固定基线。
BENCHMARK(benchmarkReadQc)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
