# Agent Note: 读入缓冲不再整块零初始化

Status: implemented

## Problem

`FastqReader` 的取数循环是「`batch.buffer().resize(kCurrentSize + chunk)` →
`readSome()` 覆盖全部新字节」。`std::vector::resize(n)` 会把新增元素值初始化，
对 `char` 就是整块 `memset`。按 64 KiB 块实测该 memset 约 8 ms/GiB，4 MiB 块
约 30 ms/GiB（大块跨出 L2 后进入冷内存带宽）；相对解析吞吐（数百 MiB/s）折算
约 0.2%–0.8%。收益不大，但完全免费：读入路径本来就是「先 resize、后整块覆盖」。

C++23 的 `resize_and_overwrite` 只存在于 `std::string`，`std::vector` 没有；
把缓冲改成 `std::string` 会扩散到 `FastqBatch` 的公共 API 与所有 `.data()`
调用点。

## Decision

在 `fqtools/io/fastq_io.h` 新增 `NoInitAllocator<T>` 与 `ByteBuffer`
（`std::vector<char, NoInitAllocator<char>>`）：

- 分配器把「无参 construct」（`resize` 的默认构造路径）实现为空操作；
  `char` 这类隐式生存期类型由取得存储即创建对象（[basic.life]），不需要
  placement new。
- 带参 construct 仍是真实构造，因此 `assign` / `insert` / `push_back` /
  `vector(n, value)` 的复制语义完全不变——这是本方案与「无脑空实现 construct」
  的关键区别，后者会让 `assign` 静默不复制。
- `FastqBatch::buffer()` 返回类型改为 `ByteBuffer&`，`buffer_` 与 reader 内部的
  `remainder` 同步换型。
- 契约写进注释：只用于 trivial 隐式生存期类型；`resize` 之后、写入之前的字节
  值不确定，禁止读取。

## Alternatives considered

- **改用 `std::string` 缓冲 + `resize_and_overwrite`**：标准且更「正统」，但
  `FastqBatch::buffer()` 的公共返回类型、`std::span<char>` 构造点、序列化器
  （`appendSerializedRecords(std::vector<char>&)`）都要跟着改，改动面远大于收益。
- **自定义 `ByteBuffer` 类（`unique_ptr<char[]>` + size）**：同样能免初始化，
  但要重新实现 `reserve`/`capacity`/`insert`/迭代器等一整套 vector 语义，
  且同样破坏公共 API。
- **什么都不做**：可接受（<1%），但零成本且可测，故采用。

## Consequences

- 每 GiB 输入省下 8–30 ms 的 memset；端到端提升在测量噪声量级（<1%）。
- `FastqBatch::buffer()` 的类型变了：对外部代码是源码级不兼容（`std::vector<char>&`
  绑定会失败）；仓库内所有调用点只用 `data()/size()/empty()/resize()/capacity()`，
  无需改动。
- 分配器对元素类型的约束靠注释表达，误用到非 trivial 类型会得到未构造对象。

## Verification

- `./scripts/core/build --dev` + `./scripts/core/test --build-dir build/clang-debug`：
  14/14 通过（含新增 `NoInitAllocatorTest` 3 例）。
- `./scripts/core/build --sanitizer asan` + 测试：14/14 通过，无内存错误。
- `./scripts/core/lint check`、`./scripts/core/lint format` 通过。
- 微基准（`clang++ -std=c++23 -stdlib=libc++ -O2`，1 GiB 累计 resize）：
  `vector<char>` 7.8–8.7 ms/GiB（64 KiB）、21–31 ms/GiB（4 MiB）；
  `NoInitAllocator<char>` 两侧均 ≈0 ms/GiB。
- 契约测试 `tests/unit/io/test_byte_buffer.cpp`：resize 不清零、assign/insert
  仍复制、`FastqBatch` 缓冲复用容量。
