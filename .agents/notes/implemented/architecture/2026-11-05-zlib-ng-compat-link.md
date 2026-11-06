# Agent Note: zlib-ng compat 链接修复

Status: implemented

## Problem

`conanfile.py` 声明了 `zlib-ng/2.3.3`，但 zlib-ng 默认 `zlib_compat=False`，只安装 `zlib-ng.h` 与 `libz-ng`。项目实际通过 `find_package(ZLIB)` 和 `<zlib.h>` 使用 zlib 兼容 API，因此 CMake 静默找到了系统 `libz.so.1`，声明的 zlib-ng 从未进入二进制。

`perf record` 显示 gzip writer 约 63% 的周期在 `libz.so.1.3` 内部；同机 gzip reader 约 892.9k reads/s，gzip-6 writer 约 51.4k reads/s。依赖声明与实际链接不一致，使“使用 zlib-ng”的性能叙事失去依据。

## Decision

在 `build-config/dependencies/conanfile.py` 的 `configure()` 中显式启用：

```python
self.options["zlib-ng"].zlib_compat = True
self.options["zlib-ng"].with_gzfileop = True
```

这样 Conan 生成 `ZLIB::ZLIB` 目标，现有 `gzFile` / `gzwrite` / `gzread` 代码不变，实际静态链接 zlib-ng。选项写进 recipe，而不是依赖本地 `conan install -o`，保证 CI、开发机和发布构建同源。`CMakeLists.txt` 额外检查 `ZLIB::ZLIB` 是否暴露 `ZLIB_COMPAT`，不暴露时 warning，避免未来再次静默回退。

## Alternatives considered

- **迁移到 `zlib-ng.h` 的 `zng_*` API** — 被否：需要改 reader/writer 全部调用点和类型，收益与 compat 模式相同，改动面明显更大。
- **保留系统 zlib 并删除未使用的 Conan 依赖** — 被否：放弃 zlib-ng 的实现与指令集优化，gzip 吞吐更低，也与 README/架构文档的声明不符。
- **只在构建命令里传 `-o zlib-ng/*:zlib_compat=True`** — 被否：其他入口仍会静默回退，问题会复发。

## Consequences

- **收益**：同机、同源码、A/B 交替复测下 gzip-6 writer 18.36–18.60 s → 7.13–7.16 s（约 2.6x，同一 plain 输入，归因较干净；早期非交替测量为 19.46 s → 7.89 s，量级一致）。gzip reader 固定同一流的 5 轮 A/B 为 1.44–1.47x（zlib-ng 流 841.9→574.7 ms，系统 zlib 流 1,054.5→732.2 ms）；早期直接 benchmark 的 921→555 ms 混入流差异，固定旧流 `stat -t 1` 的 1.50–1.51→1.25–1.26 s 被统计计算稀释。精确 reader 倍数不应写成 2.0x。
- **代价**：首次 `conan install` 需要构建 zlib-ng compat 包；二进制静态包含 zlib-ng，但 `ldd` 不再依赖系统 libz。
- **后续**：gzip 串行结构已于 2026-11-06 由 opt-in 多 member 块级并行解决（默认关闭），见 [gzip 块级并行](./2026-11-06-gzip-block-parallel.md)。

## Verification

```bash
./scripts/core/build --preset clang-release --no-lto
ldd build/clang-release/FastQTools | grep -E 'libz|zlib' || true
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release --output-dir /tmp/fq-zlibng-runner \
  --repetitions 2 --filter 'benchmarkReaderGzip|benchmarkWriter/gzip_6_batch'
```

编译命令包含 `-DZLIB_COMPAT -DWITH_GZFILEOP` 与 zlib-ng 的 include 路径；`ldd` 不再出现 `libz.so.1`。`run_benchmarks.py` 摘要：gzip reader 555.380 ms / 1,800,686.5 reads/s，gzip-6 writer 7,889.985 ms / 126,758.1 reads/s，wall CV 均低于 2%。reader 的系统 zlib 对照为两轮 919/923 ms（流不同）；同一旧流的 `stat -t 1` 对照为 1.50–1.51 s vs 1.25–1.26 s。`gzip -t` 与 `zcat | cmp` 验证产物可由系统工具读取。
