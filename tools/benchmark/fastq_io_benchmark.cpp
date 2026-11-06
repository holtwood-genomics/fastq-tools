#include <chrono>
#include <cstddef>
#include <cstdint>

#include "benchmark_support.h"

#include <benchmark/benchmark.h>

namespace fq::benchmark {

namespace {

void benchmarkReader(::benchmark::State& state) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();

    const auto wallStart = std::chrono::steady_clock::now();
    std::size_t observedReads = 0;
    for (auto _ : state) {
        fq::io::FastqReader reader(inputPath.string());
        fq::io::FastqBatch batch(4 * 1024 * 1024, kBenchmarkBatchSize);
        std::size_t totalReads = 0;
        while (reader.nextBatch(batch, kBenchmarkBatchSize)) {
            totalReads += batch.size();
        }
        observedReads = totalReads;
        ::benchmark::DoNotOptimize(totalReads);
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    if (observedReads != kBenchmarkReadCount) {
        state.SkipWithError("plain input read count does not match the benchmark dataset");
        return;
    }
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    state.counters["observed_reads"] = static_cast<double>(observedReads);
}

/// gzip 输入与 plain 输入使用同一份未压缩数据，MiB/s 按未压缩字节计算，
/// 这样两种输入的吞吐可以直接比较。
void benchmarkReaderGzip(::benchmark::State& state) {
    const auto inputPath = BenchmarkDataset::gzipPath();
    const auto inputBytes = BenchmarkDataset::fileSize();

    const auto wallStart = std::chrono::steady_clock::now();
    std::size_t observedReads = 0;
    for (auto _ : state) {
        fq::io::FastqReader reader(inputPath.string());
        fq::io::FastqBatch batch(4 * 1024 * 1024, kBenchmarkBatchSize);
        std::size_t totalReads = 0;
        while (reader.nextBatch(batch, kBenchmarkBatchSize)) {
            totalReads += batch.size();
        }
        observedReads = totalReads;
        ::benchmark::DoNotOptimize(totalReads);
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    if (observedReads != kBenchmarkReadCount) {
        state.SkipWithError("gzip input read count does not match the benchmark dataset");
        return;
    }
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    state.counters["observed_reads"] = static_cast<double>(observedReads);
    state.counters["compressed_input_bytes"] =
        static_cast<double>(BenchmarkDataset::compressedFileSize());
    state.counters["fixed_stream"] = BenchmarkDataset::usesFixedGzip() ? 1.0 : 0.0;
}

void benchmarkWriter(::benchmark::State& state, const WriterBenchmarkSpec& spec, bool batchApi) {
    const auto& batches = loadBenchmarkBatches();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto outputPath = writerOutputPath(spec, batchApi ? "batch" : "single");

    const auto wallStart = std::chrono::steady_clock::now();
    std::uint64_t observedWrittenBytes = 0;
    for (auto _ : state) {
        fq::io::FastqWriterOptions options;
        options.compression = spec.compression;
        options.compressionLevel = spec.compressionLevel;
        fq::io::FastqWriter writer(outputPath.string(), options);
        std::uint64_t writtenBytes = 0;

        if (batchApi) {
            for (const auto& batch : batches) {
                writtenBytes += writer.write(batch);
            }
        } else {
            for (const auto& batch : batches) {
                for (const auto& record : batch) {
                    writer.write(record);
                }
            }
            writtenBytes = writer.totalUncompressedBytes();
        }

        // 生产路径在 finish() 中完成 gzip close、fsync 和原子 rename；
        // 旧基准漏掉这一段，会把 writer 吞吐算得偏高。
        writer.finish();
        observedWrittenBytes = writtenBytes;
        ::benchmark::DoNotOptimize(writtenBytes);
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    removeBenchmarkOutput(outputPath);
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes, wallEnd - wallStart);
    state.counters["includes_publish"] = 1.0;
    state.counters["compression_level"] = static_cast<double>(spec.compressionLevel);
    state.counters["written_bytes"] = static_cast<double>(observedWrittenBytes);
}

}  // namespace

BENCHMARK(benchmarkReader)->Unit(::benchmark::kMillisecond);
BENCHMARK(benchmarkReaderGzip)->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  plain_single,
                  WriterBenchmarkSpec{"plain", 6, fq::io::FastqWriterCompressionMode::None},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  plain_batch,
                  WriterBenchmarkSpec{"plain", 6, fq::io::FastqWriterCompressionMode::None},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_1_single,
                  WriterBenchmarkSpec{"gzip-1", 1, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_1_batch,
                  WriterBenchmarkSpec{"gzip-1", 1, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_6_single,
                  WriterBenchmarkSpec{"gzip-6", 6, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_6_batch,
                  WriterBenchmarkSpec{"gzip-6", 6, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_9_single,
                  WriterBenchmarkSpec{"gzip-9", 9, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_9_batch,
                  WriterBenchmarkSpec{"gzip-9", 9, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
