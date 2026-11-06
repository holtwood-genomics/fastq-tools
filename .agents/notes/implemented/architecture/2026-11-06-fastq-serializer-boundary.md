# Agent Note: FASTQ 纯序列化器与 Writer 分层

Status: implemented

## Problem

`FastqWriter::Impl` 仍同时持有记录序列化、缓冲区容量切分、gzip 模式选择、临时文件发布等职责。记录格式（`@id comment`、四行换行、空 `plus` 回退为 `+`）与“按可用容量追加整条记录”的逻辑埋在 500 行以上的 I/O 实现里：

- 记录编码契约只能通过真实文件写出间接验证，无法像 `parseFastqRecords` 一样直接单测；
- 后续新增输出后端或调整缓冲策略时，容易误碰编码格式；
- Reader 已有纯解析边界，Writer 没有对应的纯序列化边界，两侧分层不对称。

## Decision

- 新增内部 `src/io/fastq_serializer.{h,cpp}`，提供：
  - `serializedRecordSize(record)`：一条记录序列化后的精确字节数；
  - `appendSerializedRecord(dst, record)`：写入规范化 FASTQ 记录并返回末指针；
  - `appendSerializedRecords(out, records, maxBytes)`：按字节预算追加记录前缀，即使预算小于首条记录也至少追加一条，保证调用方前进。
- `FastqWriter` 只负责 sink 生命周期与缓冲：调用序列化器计算容量、追加记录、累计未压缩字节；删除原有的 `recordSize`/`appendView`/`appendRecordBytes` 私有函数。
- `appendSerializedRecords` 接收 `std::span<const FastqRecord>`，不依赖 `FastqBatch` 容器，便于直接单测和未来复用。
- 保持现有输出字节完全不变：空 `plus` 仍写 `+`，comment 仍以单个空格分隔，CRLF 不参与写出（写出统一 `\n`）。

## Alternatives considered

- **只把 `recordSize`/`appendRecordBytes` 留在 Writer 内继续复用** — 被否：仍是 Writer 私有实现，无法直接验证编码契约，也没有解决 Writer 职责过宽。
- **引入格式无关的 `ByteSink`/`BatchSink` 抽象** — 暂否：当前只有 FASTQ 一种序列化格式，没有第二个消费者；先抽出纯函数边界，不提前设计通用 sink 层级。
- **让序列化器自己管理 `std::vector<char>` 与 flush** — 被否：flush 时机、gzip 缓冲和临时文件发布属于 Writer 的 sink 职责；序列化器只做记录到字节的纯变换。

## Consequences

- 记录编码与容量切分可以脱离文件系统直接测试；Writer 的 `appendBatch` 缩短为“确保首条容量 → 追加前缀 → 累计字节”的循环。
- 新增头文件位于 `src/`，不安装、不导出；公共 API、CLI 参数、输出字节和错误语义不变。
- `appendSerializedRecords` 的 `maxBytes` 是软上限：首条记录必须完整写出，因此调用方仍需先用 `serializedRecordSize` 保证缓冲区至少容纳一条记录。

## Verification

```bash
./scripts/core/lint format
./scripts/core/lint check
./scripts/core/build --preset clang-release --no-lto
ctest --test-dir build/clang-release --output-on-failure
./scripts/core/build --sanitizer asan --no-lto
ctest --test-dir build/clang-asan --output-on-failure
./scripts/core/build --sanitizer tsan --no-lto
ctest --test-dir build/clang-tsan --output-on-failure
./scripts/core/notes verify
```

`tests/unit/io/test_fastq_serializer.cpp` 覆盖记录大小、默认/自定义 plus、comment 格式、字节预算与“至少前进一条”语义；原有 `FastqWriterTest` 继续验证真实文件写出、gzip 与原子发布路径。
