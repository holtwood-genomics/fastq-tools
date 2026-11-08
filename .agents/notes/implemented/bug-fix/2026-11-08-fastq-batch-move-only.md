# Agent Note: FastqBatch 改为 move-only，禁止拷贝悬空记录视图

Status: implemented

## Problem

`FastqBatch` 同时持有 `buffer_`（`std::vector<char>`）和 `records_`（`std::vector<FastqRecord>`，字段全为 `string_view`）。类没有声明拷贝/移动操作，编译器生成隐式拷贝：`buffer_` 被深拷贝，但 `records_` 里的视图指针原样复制，继续指向源批次的 `buffer_`。拷贝后源批次析构或被对象池复用，目标批次的记录即成为悬空读；改写源缓冲区还会让目标批次读到串数据。这与 `docs/architecture.md` 中“记录生命周期严格绑定到所属 FastqBatch”的契约冲突。

`tests/integration/test_pipeline_integration.cpp` 曾用 `std::vector<FastqBatch>{batch}` 构造，触发隐式拷贝；只因测试记录的视图指向 `TestRecordStorage` 的 `deque` 字符串才未暴露。

## Decision

- `FastqBatch` 显式删除拷贝构造与拷贝赋值，显式 `= default` 移动构造/移动赋值（`noexcept`）。用户声明拷贝会抑制隐式移动，因此移动必须显式 default。
- 移动转移 `buffer_` 的堆存储，`records_` 内的视图保持指向同一存储，移动后仍有效。
- 集成测试中的 initializer-list 构造改为先构造 `std::vector<FastqBatch>`，再 `push_back(std::move(batch))`。
- 新增编译期回归：`static_assert(!std::is_copy_constructible_v<FastqBatch>)`、`is_move_constructible_v`/`is_move_assignable_v`，并验证移动后记录视图仍读到正确内容。

## Alternatives considered

- **保留拷贝语义并在拷贝构造中按偏移 rebase 记录视图** — 被否：记录视图不一定指向 `buffer_`（自定义 Reader 可指向外部存储），偏移 rebase 无法可靠实现，还会保留一个危险入口。
- **只加注释提醒调用方不要拷贝** — 被否：隐式拷贝是编译器默认行为，注释无法阻止实际误用；move-only 与 `Pipeline`、`FastqReader`、`FastqWriter` 的既有风格一致。
- **在拷贝构造中深拷贝并重新解析** — 被否：需要保存原始字节布局且无法覆盖外部视图来源，成本与语义都不可接受。

## Consequences

- 这是 source-breaking 公共 API 变更：按值保存/传递 `FastqBatch` 的代码需改为 `std::move`。仓库内唯一受影响的是集成测试的 initializer-list。
- `std::vector<FastqBatch>` 重新分配走 noexcept 移动，不会触发拷贝回退。
- 记录视图的生命周期契约现在由类型系统保证：批次不可拷贝，逃逸风险只剩 `string_view` 本身。

## Verification

```bash
./scripts/core/build
./scripts/core/test -t Release -j 4
./scripts/core/lint format
```

`test_pipeline_smoke` 的静态断言在修复前会因 `is_copy_constructible_v<FastqBatch>` 为 true 而编译失败；修复后 14/14 通过。
