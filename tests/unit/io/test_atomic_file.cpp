#include "fqtools/error/error.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "test_helpers.h"

#include "io/atomic_file.h"
#include <gtest/gtest.h>

namespace fq::io::detail {
namespace {

auto readFile(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

auto temporaryFileCount(const std::filesystem::path& directory) -> size_t {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(AtomicFileTest, PublishesContentAndRemovesTemporaryFile) {
    fq::test::TempDirectory tempDir("atomic_file_");
    const auto target = tempDir.path() / "report.txt";
    std::ofstream(target) << "old";

    std::string expected(200 * 1024, 'x');
    expected += "done";
    writeAtomically(target, [&](std::ostream& output) { output << expected; });

    EXPECT_EQ(readFile(target), expected);
    EXPECT_EQ(temporaryFileCount(tempDir.path()), 0U);
}

TEST(AtomicFileTest, KeepsTargetAndCleansTemporaryWhenWriterThrows) {
    fq::test::TempDirectory tempDir("atomic_file_");
    const auto target = tempDir.path() / "report.txt";
    std::ofstream(target) << "old";

    EXPECT_THROW(writeAtomically(target,
                                 [](std::ostream& output) {
                                     output << "partial";
                                     throw std::runtime_error("body failure");
                                 }),
                 std::runtime_error);

    EXPECT_EQ(readFile(target), "old");
    EXPECT_EQ(temporaryFileCount(tempDir.path()), 0U);
}

TEST(AtomicFileTest, CleansTemporaryWhenRenameFails) {
    fq::test::TempDirectory tempDir("atomic_file_");
    const auto target = tempDir.path() / "target-directory";
    std::filesystem::create_directory(target);

    EXPECT_THROW(writeAtomically(target, [](std::ostream& output) { output << "data"; }),
                 fq::error::IOError);

    EXPECT_TRUE(std::filesystem::is_directory(target));
    EXPECT_EQ(temporaryFileCount(tempDir.path()), 0U);
}

TEST(AtomicFileTest, RejectsMissingTargetDirectory) {
    fq::test::TempDirectory tempDir("atomic_file_");
    const auto target = tempDir.path() / "missing" / "report.txt";

    EXPECT_THROW(writeAtomically(target, [](std::ostream& output) { output << "data"; }),
                 fq::error::IOError);
}

}  // namespace fq::io::detail
