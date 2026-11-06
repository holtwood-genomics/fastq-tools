/**
 * @file fq_statistic.cpp
 * @brief FASTQ 统计计算器实现
 * @details Calculator::Impl 内部走 ExecutionRuntime + StatisticRuntimeAdapter，
 *          合并了原 FastqStatisticCalculator 的职责（消除中间类）。
 */

#include "fqtools/error/error.h"
#include "fqtools/logging.h"
#include "fqtools/statistics/statistics_writer.h"

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "io/atomic_file.h"
#include "io/file_target.h"
#include "io/path_alias.h"
#include "processing/execution_runtime.h"
#include "statistics/fq_statistic_worker.h"
#include <fmt/format.h>
#include <fqtools/statistics/statistics_result.h>

namespace fq::statistics {

namespace {

class StatisticRuntimeAdapter {
public:
    using result_type = FqStatisticResult;

    StatisticRuntimeAdapter(int qualityEncoding,
                            size_t signatureKmerSize,
                            size_t duplicateEstimateSampleModulo)
        : qualityEncoding_(qualityEncoding),
          signatureKmerSize_(signatureKmerSize),
          duplicateEstimateSampleModulo_(duplicateEstimateSampleModulo) {}

    [[nodiscard]] auto makeResult() const -> result_type {
        return {};
    }

    [[nodiscard]] auto processBatch(const fq::io::FastqBatch& batch) const -> result_type {
        FqStatisticWorker worker(
            qualityEncoding_, signatureKmerSize_, duplicateEstimateSampleModulo_);
        return worker.calculateStats(batch);
    }

    void afterCommit(result_type& /*unused*/, std::uint64_t /*unused*/) const {}

    void merge(result_type& total, const result_type& partial) const {
        total += partial;
    }

private:
    int qualityEncoding_;
    size_t signatureKmerSize_;
    size_t duplicateEstimateSampleModulo_;
};

// 先写同目录临时文件、校验流状态、再原子 rename 发布——
// 与 FastqWriter 共用 io/atomic_file.h：磁盘满等写失败抛 IOError，绝不产出半截报告
void writeToDestination(const std::string& target,
                        const std::function<void(std::ostream&)>& writeBody) {
    if (target == "-") {
        writeBody(std::cout);
        std::cout.flush();
        if (!std::cout) {
            throw fq::error::IOError(target, EIO);
        }
        return;
    }
    if (fq::io::isSpecialFileTarget(target)) {
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            throw fq::error::IOError(target, errno);
        }
        writeBody(out);
        out.flush();
        if (!out.good()) {
            throw fq::error::IOError(target, EIO);
        }
        return;
    }
    fq::io::detail::writeAtomically(target, writeBody);
}

}  // namespace

void writeStatisticsOutputs(const StatisticOptions& options, const FqStatisticResult& result) {
    // 任一输出目标为 '-' 都会写 stdout；多个 '-' 目标会把不同格式拼接在同一个流上
    // （如 JSON 后跟 TSV），下游无法解析。必须保证最多一个 '-' 目标。
    const size_t stdoutTargets = static_cast<size_t>(options.outputStatPath == "-") +
        static_cast<size_t>(options.jsonOutputPath == "-") +
        static_cast<size_t>(options.signatureReportPath == "-");
    if (stdoutTargets > 1) {
        throw fq::error::ConfigurationError(
            "at most one of --output/--json/--signature-report may be '-' (stdout)");
    }

    // 报告经"临时文件 + rename"发布，会整体覆盖同名目标：目标指向输入 FASTQ
    // （stat -i x -o x）或彼此重合时静默毁掉原文件，必须在写出前拒绝
    std::vector<const std::string*> fileTargets;
    for (const auto* path :
         {&options.outputStatPath, &options.jsonOutputPath, &options.signatureReportPath}) {
        if (!path->empty() && *path != "-") {
            fileTargets.push_back(path);
        }
    }
    for (const auto* target : fileTargets) {
        if (fq::io::pathsAlias(options.inputFastqPath, *target)) {
            throw fq::error::ConfigurationError(fmt::format(
                "report target '{}' aliases the input FASTQ '{}'; choose a different output path",
                *target,
                options.inputFastqPath));
        }
    }
    for (size_t i = 0; i < fileTargets.size(); ++i) {
        for (size_t j = i + 1; j < fileTargets.size(); ++j) {
            if (fq::io::pathsAlias(*fileTargets[i], *fileTargets[j])) {
                throw fq::error::ConfigurationError(
                    fmt::format("report targets '{}' and '{}' refer to the same file",
                                *fileTargets[i],
                                *fileTargets[j]));
            }
        }
    }

    StatisticsWriterOptions writerOptions;
    writerOptions.inputFastqPath = options.inputFastqPath;
    writerOptions.qualityEncoding = options.qualityEncoding;
    writerOptions.duplicateEstimateSampleModulo = options.duplicateEstimateSampleModulo;
    writerOptions.signatureReportPath = options.signatureReportPath;
    writerOptions.jsonOutputPath = options.jsonOutputPath;
    writerOptions.maxReportedSignatures = options.maxReportedSignatures;

    StatisticsWriter statsWriter(writerOptions);
    if (!options.outputStatPath.empty()) {
        writeToDestination(options.outputStatPath,
                           [&](std::ostream& writer) { statsWriter.write(writer, result); });
    }

    if (!options.jsonOutputPath.empty()) {
        writeToDestination(options.jsonOutputPath,
                           [&](std::ostream& writer) { statsWriter.writeJson(writer, result); });
    }

    if (!options.signatureReportPath.empty()) {
        writeToDestination(options.signatureReportPath, [&](std::ostream& writer) {
            statsWriter.writeSignature(writer, result);
        });
    }
}

class Calculator::Impl {
public:
    explicit Impl(StatisticOptions options) : options_(std::move(options)) {
        options_.processing.validate();
    }

    void run() {
        fq::logging::info("Starting FASTQ statistics generation for '{}' using execution runtime.",
                          options_.inputFastqPath);

        processing::ExecutionRuntime runtime;
        processing::ExecutionRuntimeRequest plan;
        plan.inputPath = options_.inputFastqPath;
        plan.options = options_.processing;

        auto outcome =
            runtime.execute(plan,
                            StatisticRuntimeAdapter{options_.qualityEncoding,
                                                    options_.signatureKmerSize,
                                                    options_.duplicateEstimateSampleModulo});
        auto finalResult = std::move(outcome.result);

        fq::logging::info("Execution runtime finished. Aggregated results from all batches.");
        writeStatisticsOutputs(options_, finalResult);
        fq::logging::info("Statistics report saved to '{}'", options_.outputStatPath);
    }

private:
    StatisticOptions options_;
};

Calculator::Calculator(StatisticOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
Calculator::~Calculator() = default;
Calculator::Calculator(Calculator&&) noexcept = default;
auto Calculator::operator=(Calculator&&) noexcept -> Calculator& = default;

void Calculator::run() {
    impl_->run();
}

}  // namespace fq::statistics
