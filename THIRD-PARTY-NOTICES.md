# 第三方组件许可证说明

FastQTools 本体以 MIT 许可证发布，完整文本见同目录 [`LICENSE`](LICENSE)。

发布产物包含或链接以下第三方组件（具体版本由仓库 `conan.lock` 锁定）：

| 组件 | 用途 | 许可证 |
|------|------|--------|
| [oneTBB](https://github.com/uxlfoundation/oneTBB) | 并行流水线运行时 | Apache-2.0 |
| [hwloc](https://github.com/open-mpi/hwloc) | oneTBB 的硬件拓扑依赖 | BSD-3-Clause |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) | gzip 压缩 | Zlib |
| [fmt](https://github.com/fmtlib/fmt) | 格式化（header-only） | MIT |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 命令行解析（header-only） | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 序列化（header-only） | MIT |

Linux 动态链接产物按平台附带 C++ 运行库：

- x86_64：libc++ / libc++abi（Apache-2.0 WITH LLVM-exception，<https://llvm.org/LICENSE.txt>）
- aarch64：libstdc++（GPL-3.0-or-later WITH GCC Runtime Library Exception，<https://www.gnu.org/licenses/gcc-exception-3.1.html>）

glibc 等系统库由目标系统提供，不随产物分发。

各许可证完整文本可从上述组件的上游仓库获取。
