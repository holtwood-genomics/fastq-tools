#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common_options.h"
#include <cxxopts.hpp>

#include "cli/filter_plan.h"
#include <fqtools/fq.h>
#include <gtest/gtest.h>

namespace fq::cli {
namespace {

class CapturingPipeline {
public:
    void setInputPath(const std::string& inputPath) {
        inputPath_ = inputPath;
    }

    void setOutputPath(const std::string& outputPath) {
        outputPath_ = outputPath;
    }

    void setReader(std::unique_ptr<fq::io::IReader> /*reader*/) {}

    void setWriter(std::unique_ptr<fq::io::IWriter> /*writer*/) {}

    void setProcessingOptions(const fq::processing::ProcessingOptions& options) {
        options_ = options;
    }

    void setOutputOptions(const fq::processing::OutputOptions& options) {
        outputOptions_ = options;
    }

    void addReadMutator(std::unique_ptr<fq::processing::ReadMutatorInterface> mutator) {
        mutators_.push_back(std::move(mutator));
    }

    void addReadPredicate(std::unique_ptr<fq::processing::ReadPredicateInterface> predicate) {
        predicates_.push_back(std::move(predicate));
    }

    auto run() -> fq::processing::ProcessingStatistics {
        return {};
    }

    std::string inputPath_;
    std::string outputPath_;
    fq::processing::ProcessingOptions options_;
    fq::processing::OutputOptions outputOptions_;
    std::vector<std::unique_ptr<fq::processing::ReadPredicateInterface>> predicates_;
    std::vector<std::unique_ptr<fq::processing::ReadMutatorInterface>> mutators_;
};

}  // namespace

TEST(FilterPlanTest, BuildsAndAppliesRepresentativeFilterOptions) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {
        "filter",
        "--input",
        "input.fastq",
        "--output",
        "output.fastq",
        "--threads",
        "2",
        "--batch-size",
        "3",
        "--min-length",
        "4",
        "--trim-quality",
        "20",
        "--trim-mode",
        "three",
        "--adapter-seq",
        "TTAA",
        "--trim-poly-g",
        "4",
    };

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    auto plan = buildFilterPlan(parsed, common);
    CapturingPipeline pipeline;
    plan.applyTo(pipeline);

    EXPECT_EQ(pipeline.inputPath_, "input.fastq");
    EXPECT_EQ(pipeline.outputPath_, "output.fastq");
    EXPECT_EQ(pipeline.options_.threadCount, 2U);
    EXPECT_EQ(pipeline.options_.batchSize, 3U);
    EXPECT_EQ(pipeline.outputOptions_.gzipBlockBytes, 0U);

    ASSERT_EQ(pipeline.predicates_.size(), 1U);
    EXPECT_NE(dynamic_cast<fq::processing::MinLengthPredicate*>(pipeline.predicates_[0].get()),
              nullptr);

    ASSERT_EQ(pipeline.mutators_.size(), 3U);
    EXPECT_NE(dynamic_cast<fq::processing::AdapterTrimmer*>(pipeline.mutators_[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<fq::processing::PolyTailTrimmer*>(pipeline.mutators_[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<fq::processing::QualityTrimmer*>(pipeline.mutators_[2].get()), nullptr);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "II!!", "+"};
    pipeline.mutators_[2]->process(read);
    EXPECT_EQ(read.seq, "AC");
    EXPECT_EQ(read.qual, "II");
}

TEST(FilterPlanTest, BuildsAdapterAndPolyXTailMutatorsFromRepeatableOptions) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {
        "filter",
        "--input",
        "input.fastq",
        "--output",
        "output.fastq",
        "--adapter-seq",
        "TTAA",
        "--adapter-seq",
        "GGCC",
        "--adapter-min-overlap",
        "4",
        "--adapter-max-mismatches",
        "1",
        "--trim-poly-x",
        "5",
    };

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    auto plan = buildFilterPlan(parsed, common);
    CapturingPipeline pipeline;
    plan.applyTo(pipeline);

    ASSERT_EQ(pipeline.mutators_.size(), 2U);

    auto* const adapterTrimmer =
        dynamic_cast<fq::processing::AdapterTrimmer*>(pipeline.mutators_[0].get());
    ASSERT_NE(adapterTrimmer, nullptr);

    fq::io::FastqRecord read1{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    adapterTrimmer->process(read1);
    EXPECT_EQ(read1.seq, "ACGT");
    EXPECT_EQ(read1.qual, "IIII");

    fq::io::FastqRecord read2{"read2", {}, "ACGTGGCC", "IIIIIIII", "+"};
    adapterTrimmer->process(read2);
    EXPECT_EQ(read2.seq, "ACGT");
    EXPECT_EQ(read2.qual, "IIII");

    auto* const polyTailTrimmer =
        dynamic_cast<fq::processing::PolyTailTrimmer*>(pipeline.mutators_[1].get());
    ASSERT_NE(polyTailTrimmer, nullptr);

    fq::io::FastqRecord polyXRead{"polyX", {}, "ACGTAAAAA", "IIIIIIIII", "+"};
    polyTailTrimmer->process(polyXRead);
    EXPECT_EQ(polyXRead.seq, "ACGT");
    EXPECT_EQ(polyXRead.qual, "IIII");

    fq::io::FastqRecord shortPolyXRead{"shortPolyX", {}, "ACGTTTT", "IIIIIII", "+"};
    polyTailTrimmer->process(shortPolyXRead);
    EXPECT_EQ(shortPolyXRead.seq, "ACGTTTT");
    EXPECT_EQ(shortPolyXRead.qual, "IIIIIII");
}

TEST(FilterPlanTest, BuildsLengthTrimmerAfterQualityTrimAndKeepsPrefix) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {
        "filter",
        "--input",
        "input.fastq",
        "--output",
        "output.fastq",
        "--trim-quality",
        "20",
        "--trim-length",
        "3",
        "--max-length",
        "10",
    };

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    auto plan = buildFilterPlan(parsed, common);
    CapturingPipeline pipeline;
    plan.applyTo(pipeline);

    ASSERT_EQ(pipeline.predicates_.size(), 1U);
    EXPECT_NE(dynamic_cast<fq::processing::MaxLengthPredicate*>(pipeline.predicates_[0].get()),
              nullptr);

    ASSERT_EQ(pipeline.mutators_.size(), 2U);
    EXPECT_NE(dynamic_cast<fq::processing::QualityTrimmer*>(pipeline.mutators_[0].get()), nullptr);
    auto* const lengthTrimmer =
        dynamic_cast<fq::processing::LengthTrimmer*>(pipeline.mutators_[1].get());
    ASSERT_NE(lengthTrimmer, nullptr);

    fq::io::FastqRecord read{"read1", {}, "ACGTAA", "IIIIII", "+"};
    lengthTrimmer->process(read);
    EXPECT_EQ(read.seq, "ACG");
    EXPECT_EQ(read.qual, "III");
}

TEST(FilterPlanTest, RejectsZeroTrimLength) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {
        "filter", "--input", "input.fastq", "--output", "output.fastq", "--trim-length", "0"};

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
}

TEST(FilterPlanTest, RejectsUnsupportedQualityEncoding) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {
        "filter", "--input", "input.fastq", "--output", "output.fastq", "--quality-encoding", "40"};

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
}

TEST(FilterPlanTest, RejectsOutOfRangeMaxNRatioAndConflictingLengths) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    {
        const std::vector<std::string> args = {
            "filter", "--input", "input.fastq", "--output", "output.fastq", "--max-n-ratio", "1.5"};

        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }

        const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
        const auto common = CommonCliOptions::parse(parsed);

        EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
    }

    {
        const std::vector<std::string> args = {"filter",
                                               "--input",
                                               "input.fastq",
                                               "--output",
                                               "output.fastq",
                                               "--min-length",
                                               "10",
                                               "--max-length",
                                               "3"};

        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }

        const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
        const auto common = CommonCliOptions::parse(parsed);

        EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
    }
}

TEST(FilterPlanTest, RejectsNegativeQualityThresholds) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {"filter",
                                           "--input",
                                           "input.fastq",
                                           "--output",
                                           "output.fastq",
                                           "--min-quality",
                                           "-1",
                                           "--trim-quality",
                                           "-2"};

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
}

TEST(CommonCliOptionsTest, DefaultsToAutoThreadCount) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);

    const std::vector<std::string> args = {
        "filter", "--input", "input.fastq", "--output", "out.fastq"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_GE(common.threadCount, 1U);
    EXPECT_LE(common.threadCount, 8U);
}

TEST(CommonCliOptionsTest, AutoThreadCountFallsBackForSmallFiles) {
    const auto smallFile = std::filesystem::temp_directory_path() /
        ("fqtools-cli-auto-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fastq");
    {
        std::ofstream output(smallFile, std::ios::binary);
        output << "x";
    }

    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    const std::vector<std::string> args = {
        "filter", "--input", smallFile.string(), "--output", "out.fastq"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    std::error_code error;
    std::filesystem::remove(smallFile, error);
    EXPECT_EQ(common.threadCount, 1U);
}

TEST(CommonCliOptionsTest, AutoThreadCountUsesCapForLargeFiles) {
    const auto largeFile = std::filesystem::temp_directory_path() /
        ("fqtools-cli-auto-large-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fastq");
    {
        std::ofstream output(largeFile, std::ios::binary);
    }
    std::filesystem::resize_file(largeFile, 65ULL * 1024ULL * 1024ULL);

    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    const std::vector<std::string> args = {
        "filter", "--input", largeFile.string(), "--output", "out.fastq"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    const auto hardware = std::thread::hardware_concurrency();
    const auto expected =
        std::max<std::size_t>(1, std::min<std::size_t>(hardware == 0 ? 1 : hardware, 8));

    std::error_code error;
    std::filesystem::remove(largeFile, error);
    EXPECT_EQ(common.threadCount, expected);
}

TEST(CommonCliOptionsTest, AutoThreadCountCapsStdin) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    const std::vector<std::string> args = {"filter", "--input", "-", "--output", "out.fastq"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    const auto hardware = std::thread::hardware_concurrency();
    const auto expected = std::min<std::size_t>(std::max<std::size_t>(1, hardware), 4);
    EXPECT_EQ(common.threadCount, expected);
}

TEST(CommonCliOptionsTest, ExplicitThreadCountIsPreserved) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    const std::vector<std::string> args = {
        "filter", "--input", "input.fastq", "--output", "out.fastq", "--threads", "4"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(CommonCliOptions::parse(parsed).threadCount, 4U);
}

TEST(CommonCliOptionsTest, RejectsZeroOrInvalidThreadCount) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);

    const auto parseThreads = [&options](const std::string& value) {
        const std::vector<std::string> args = {
            "filter", "--input", "input.fastq", "--output", "out.fastq", "--threads", value};
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
        return CommonCliOptions::parse(parsed);
    };

    EXPECT_THROW(static_cast<void>(parseThreads("0")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parseThreads("-1")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parseThreads("+4")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parseThreads("two")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parseThreads("18446744073709551616")), std::invalid_argument);
}

// 超大 MB/GB 值换算字节会回绕 size_t，必须在换算前拒绝
TEST(CommonCliOptionsTest, ParsesGzipBlockMb) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);
    const std::vector<std::string> args = {
        "filter", "--input", "input.fastq", "--output", "out.fastq.gz", "--gzip-block-mb", "4"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    ASSERT_TRUE(common.gzipBlockMb.has_value());
    EXPECT_EQ(common.gzipBlockMb.value(), 4U);
    EXPECT_EQ(common.toOutputOptions().gzipBlockBytes, 4ULL * 1024ULL * 1024ULL);
    const auto plan = buildFilterPlan(parsed, common);
    EXPECT_EQ(plan.outputOptions.gzipBlockBytes, 4ULL * 1024ULL * 1024ULL);
}

TEST(CommonCliOptionsTest, RejectsOverflowingGzipBlockMb) {
    CommonCliOptions common;
    common.gzipBlockMb = 1025;

    EXPECT_THROW(static_cast<void>(common.toOutputOptions()), std::invalid_argument);
}

// 超大 MB/GB 值换算字节会回绕 size_t，必须在换算前拒绝
TEST(CommonCliOptionsTest, RejectsOverflowingBatchCapacityMb) {
    CommonCliOptions common;
    common.batchCapacityMb = std::numeric_limits<size_t>::max();

    EXPECT_THROW(static_cast<void>(common.toProcessingOptions()), std::invalid_argument);
}

TEST(CommonCliOptionsTest, RejectsOverflowingMemoryLimitGb) {
    CommonCliOptions common;
    common.memoryLimitGb = std::numeric_limits<size_t>::max();

    EXPECT_THROW(static_cast<void>(common.toProcessingOptions()), std::invalid_argument);
}

// 合法值不受上限影响，换算正确
TEST(CommonCliOptionsTest, ConvertsInRangeCapacityAndLimitToBytes) {
    CommonCliOptions common;
    common.batchCapacityMb = 8;
    common.memoryLimitGb = 2;

    const auto opts = common.toProcessingOptions();

    ASSERT_TRUE(opts.batchCapacityBytes.has_value());
    EXPECT_EQ(opts.batchCapacityBytes.value(), 8ULL * 1024ULL * 1024ULL);
    ASSERT_TRUE(opts.memoryLimitBytes.has_value());
    EXPECT_EQ(opts.memoryLimitBytes.value(), 2ULL * 1024ULL * 1024ULL * 1024ULL);
}


// 回归：0 最小重叠会让 1 字符 overlap + 默认 1 错配必然命中，静默剪掉每条 read 的 3' 端
TEST(FilterPlanTest, RejectsZeroAdapterMinOverlap) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {"filter",
                                           "--input",
                                           "input.fastq",
                                           "--output",
                                           "output.fastq",
                                           "--adapter-seq",
                                           "TTAA",
                                           "--adapter-min-overlap",
                                           "0"};

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
}

// 回归：允许错配数 >= 最小重叠时，重叠比对退化为必然命中（空语义）
TEST(FilterPlanTest, RejectsAdapterMismatchesAtLeastMinOverlap) {
    cxxopts::Options options("filter", "Filter and trim FastQ files");
    CommonCliOptions::addOptions(options);
    addFilterPlanOptions(options);

    const std::vector<std::string> args = {"filter",
                                           "--input",
                                           "input.fastq",
                                           "--output",
                                           "output.fastq",
                                           "--adapter-seq",
                                           "TTAA",
                                           "--adapter-min-overlap",
                                           "3",
                                           "--adapter-max-mismatches",
                                           "3"};

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    const auto parsed = options.parse(static_cast<int>(argv.size()), argv.data());
    const auto common = CommonCliOptions::parse(parsed);

    EXPECT_THROW(static_cast<void>(buildFilterPlan(parsed, common)), std::invalid_argument);
}

// 回归：报告目标与 FASTQ 输入/输出或彼此别名时，报告的"临时文件 + rename"发布
// 会静默覆盖输入文件或已产出的结果文件，必须在参数阶段拒绝
TEST(ValidateReportTargetsTest, RejectsReportAliasingInput) {
    EXPECT_THROW(validateReportTargets("in.fastq", "out.fastq", {"in.fastq"}),
                 fq::error::ConfigurationError);
}

TEST(ValidateReportTargetsTest, RejectsReportAliasingFastqOutput) {
    EXPECT_THROW(validateReportTargets("in.fastq", "out.fastq", {"out.fastq"}),
                 fq::error::ConfigurationError);
}

TEST(ValidateReportTargetsTest, RejectsDuplicateReportTargets) {
    EXPECT_THROW(validateReportTargets("in.fastq", "out.fastq", {"r.tsv", "./r.tsv"}),
                 fq::error::ConfigurationError);
    EXPECT_THROW(validateReportTargets("in.fastq", "out.fastq", {"a.tsv", "b.tsv", "a.tsv"}),
                 fq::error::ConfigurationError);
}

TEST(ValidateReportTargetsTest, AcceptsDistinctAndInactiveTargets) {
    EXPECT_NO_THROW(
        validateReportTargets("in.fastq", "out.fastq", {"r.tsv", "r.json", "", "-", "-"}));
}

}  // namespace fq::cli
