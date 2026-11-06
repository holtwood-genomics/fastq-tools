# Agent Note: 统一临时文件与原子发布协议

Status: implemented

## Problem

`FastqWriter` 和统计报告都在实现“同目录临时文件 → 写入 → rename 发布”，但细节已经分叉：

- `FastqWriter` 用 `O_EXCL` 创建并复用 fd，写入失败时停止压缩线程、关闭 fd、删除临时文件；发布前对数据 fd 做 best-effort `fsync`，rename 后同步父目录。
- `fq_statistic.cpp` 自己拼接 `.tmp-pid-counter` 路径，用 `std::ofstream` 写入，只做 flush 后 rename；没有共享的 `O_EXCL`/fd 生命周期、数据 fsync 或父目录同步，异常清理逻辑也重复一份。

这类协议一旦分叉，后续很容易出现“一个写出路径会在磁盘满时留下半截目标，另一个不会”或“rename 成功但目录项未落盘”的静默差异。统计报告与 FASTQ 输出都承诺失败不覆盖已有目标，应该共享同一套发布语义。

## Decision

- 新增内部 `src/io/atomic_file.{h,cpp}`，提供：
  - `createTemporaryFile()`：同目录 `O_EXCL` 唯一临时文件，持有 fd，`EEXIST` 重试；
  - `publishTemporaryFile()`：原子 `rename`，随后 best-effort `fsync` 父目录；
  - `removeTemporaryFile()`：异常路径的幂等清理；
  - `writeAtomically()`：通过 fd-backed `std::ostream` 写报告，成功后 fsync/close/publish，失败清理临时文件。
- `FastqWriter` 改用共享的 `createTemporaryFile()`、`publishTemporaryFile()` 与 `removeTemporaryFile()`；保留自己的 gzip/block worker 停止、fd ownership 和发布前 `fsync` 逻辑。
- 统计报告的正常文件目标改用 `fq::io::detail::writeAtomically()`；stdout 与特殊文件（`/dev/null`、FIFO 等）仍走直接写入，因为它们不支持 rename 发布。
- 临时文件仍使用 `0666` 交给 umask，保持既有权限语义；`fsync` 继续是 best-effort，兼容 tmpfs 等没有持久语义的挂载点。

## Alternatives considered

- **只把临时文件命名抽成字符串 helper** — 被否：能减少几行重复，但无法统一 fd 生命周期、失败清理和 rename 后父目录同步，协议仍会漂移。
- **统计报告复用 `std::ofstream` + 共享 rename helper** — 被否：临时文件会先关闭再按路径重开，重新引入 TOCTOU 窗口；与 `FastqWriter` 的 `O_EXCL` 协议不一致。
- **把 `FastqWriter` 的全部发布逻辑抽象成通用 `AtomicFile` 类** — 暂否：writer 还要管理 gzip 线程、特殊文件直写和多 fd 所有权，强行抽象会掩盖这些生命周期差异；先共享最小协议原语。
- **统计报告直接写内存 `ostringstream` 再一次性写 fd** — 被否：报告大小随 read 长度/位置统计增长，内存峰值不可控。

## Consequences

- 统计报告现在与 FASTQ 输出共享唯一临时文件命名、`O_EXCL` 创建、rename 发布和错误清理路径；报告写入、flush、close 或 rename 失败都不会覆盖原目标。
- `FastqWriter` 的行为保持不变，只是把通用协议移动到内部模块；gzip worker 的停止顺序仍由 writer 自己负责。
- `atomic_file` 是 `src/` 内部头，不安装、不导出；`fq_modern_io` 继续把 `fq_error` 作为私有依赖。
- `writeAtomically` 使用 64 KiB fd-backed 缓冲区，避免报告格式化的细碎写入放大系统调用。

## Verification

```bash
./scripts/core/lint format
./scripts/core/lint check
./scripts/core/lint cppcheck -b build/clang-release
./scripts/core/build --preset clang-release --no-lto
ctest --test-dir build/clang-release --output-on-failure
./scripts/core/build --sanitizer asan --no-lto
ctest --test-dir build/clang-asan --output-on-failure
./scripts/core/build --sanitizer tsan --no-lto
ctest --test-dir build/clang-tsan --output-on-failure
./scripts/core/notes verify
```

直接单测覆盖：临时文件发布后内容正确且无残留、writer 抛异常时保留原目标并清理临时文件、目标目录不存在时抛 `IOError`。全量测试同时覆盖 `FastqWriter` 的串行/块级 gzip、统计报告特殊文件直写和 CLI 写出路径。
