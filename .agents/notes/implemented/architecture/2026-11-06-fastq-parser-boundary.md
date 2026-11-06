# Agent Note: FASTQ 纯解析器与 Reader I/O 分层

Status: implemented

## Problem

`FastqReader::nextBatch` 同时负责文件/stdin 打开、gzip sniff、动态读取量估算、跨批 remainder、FASTQ 四行解析、CRLF 处理、格式校验和 maxBuffer 守卫。解析逻辑埋在 477 行的 I/O 实现里，导致：

- 解析边界（消费到哪、残片在哪、EOF 与不完整记录的区别）只能通过真实文件间接测试；
- 调整 I/O 缓冲策略时容易误碰解析路径；
- 新增格式校验或性能改动时，review 需要同时理解文件描述符生命周期和 string_view 生命周期。

## Decision

- 新增内部 `src/io/fastq_parser.{h,cpp}`，提供 `detail::parseFastqRecords(data, size, maxRecords, eof, batch)`。
- 解析器只处理连续缓冲区：
  - 返回 `consumedBytes`（最后一条完整记录之后的偏移）与 `recordCount`；
  - 记录字段仍是指向输入缓冲区的 `string_view`，不复制；
  - `!eof` 时把尾部不完整记录留在未消费区，`eof` 时抛 `FormatError`；
  - 保持现有错误文案、CRLF 处理、跳过前导空行、`maxRecords` 停止语义。
- `FastqReader` 只保留 I/O 与缓冲职责：打开/嗅探 gzip、按目标字节数读取、维护 remainder、调用解析器、更新平均记录大小、执行 maxBuffer 守卫。
- `FastqReader` 不再包含 `fmt` 依赖；解析器负责格式错误文案。
- 新增直接单测覆盖解析器消费边界、残片、EOF、CRLF、前导空行；另加 reader 小 chunk + remainder 集成用例。

## Alternatives considered

- **只把解析循环提成 reader 的私有函数** — 被否：仍是 reader 的成员，无法独立测试，也无法在不构造文件描述符的情况下直接 fuzz。
- **引入通用 `ByteSource` 接口** — 暂否：当前只有文件/stdin/gzip 三类来源，且 gzip 嗅探与 fd 所有权已集中在 Impl；没有第二种消费者前先不抽象。
- **解析器接收 `FastqBatch&` 并清空 records** — 被否：清空策略属于 reader 的批次生命周期；纯解析器只追加记录，调用方决定何时清理。
- **让解析器返回 `std::vector<FastqRecord>`** — 被否：会破坏零拷贝视图与批次内存复用，且每次解析产生额外分配。

## Consequences

- 解析错误与成功路径的 string_view 生命周期约束更明确：调用方必须保证输入缓冲区活到 batch 使用结束。
- `FastqReader` 的代码聚焦于 I/O 策略；解析器可被单测直接调用，现由独立 `fastq_parser_core_fuzzer` 直接覆盖，不再需要经过文件系统。
- 行为保持不变，包括：
  - 只含空行的输入在 EOF 时返回 `false`；
  - 完整记录后跟不完整记录且未 EOF 时，先返回完整记录，残片进 remainder；
  - `maxRecords` 达到后不再检查后续字节；
  - 记录之间出现非法起始字节立即抛 `FormatError`。
- 新头文件在 `src/` 内部，不安装、不导出公共 API。

## Verification

```bash
./scripts/core/lint format
./scripts/core/lint check
./scripts/core/lint cppcheck -b build/clang-release
./scripts/core/build --preset clang-release --no-lto
ctest --test-dir build/clang-release --output-on-failure   # 14/14
./build/clang-release/tests/unit/test_io --gtest_filter='*Chunks*:*FastqParser*'  # 6/6
./scripts/core/build --sanitizer asan --no-lto
ctest --test-dir build/clang-asan --output-on-failure      # 14/14
./scripts/core/build --sanitizer tsan --no-lto
ctest --test-dir build/clang-tsan --output-on-failure      # 14/14

# 纯解析 fuzzer 构建与 15s smoke
cmake --build build-fuzz --target fastq_parser_core_fuzzer
./build-fuzz/fuzzers/fastq_parser_core_fuzzer tools/fuzz/corpus/ \
    -max_total_time=15 -max_len=4096
```
