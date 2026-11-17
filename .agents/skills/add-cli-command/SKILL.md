---
name: add-cli-command
description: 在 FastQTools 中新增一个 CLI 子命令的完整联动流程：command 类实现、registry 注册、CMake、测试四层（unit/integration/e2e）、docs/cli-reference.md、README 与 CHANGELOG 同步。Use when 用户要求新增/修改 FastQTools 子命令、添加 CLI 命令，或评审涉及 src/cli/commands 的改动完整性时。
---

# 新增 CLI 命令

FastQTools 的新命令不是单文件改动——`stat`/`filter` 各自牵动 **代码注册、CMake、测试四层、两处文档、CHANGELOG**。本 skill 把全部触点收敛成一张清单，防止漏一处处处报绿。

## 触点全景

| 触点 | 路径 | 动作 |
|------|------|------|
| 命令类 | `src/cli/commands/<name>_command.{h,cpp}` | 新增，继承 `fq::cli::CommandInterface` |
| 注册 | `src/cli/command_registry.cpp` | `createDefaultCommandRegistry()` 中 `registerCommand("<name>", ...)` |
| 构建 | `src/cli/CMakeLists.txt` | `.cpp/.h` 加入 `fq_cli` 源列表 |
| 单测 | `tests/unit/cli/test_command_registry.cpp` | registry 能找到新命令 |
| 单测（可选） | `tests/unit/cli/test_<plan>.cpp` | 命令有独立 plan/options 解析逻辑时，镜像 `test_filter_plan.cpp` |
| 集成 | `tests/integration/test_pipeline_integration.cpp` | 走流水线的命令补一条跨模块用例 |
| E2E | `tests/e2e/test_cli.sh` 和/或 `test_advanced_cli.py` | CLI 黑盒：真实样本 + 退出码 + 输出内容断言 |
| 参考 | `docs/cli-reference.md` | 新命令小节：示例 + 参数表（含默认值） |
| 门面 | `README.md`（、`docs/getting-started.md`） | 面向用户的入口提及 |
| 历史 | `CHANGELOG.md` `[Unreleased]` | Keep a Changelog 条目 |

## 流程

1. **动手前查否决记录**：`rg --hidden --glob '!.agents/notes/archived/**' '<关键词>' .agents/notes/`。产品面刻意收窄为 stat + filter（见 `docs/architecture.md` 项目定位与 `.agents/notes/rejected/feature/2026-07-18-full-qc-scope.md`）——「补齐 fastp 功能面」式命令先确认立项理由，再写代码。
2. **实现命令类**，契约照抄 `filter_command.cpp`：
   - `execute()` 先 `CommonCliOptions::addOptions()`，再加特有参数；`argc == 1` 或 `--help` 打印 usage 返回 0。
   - 共享参数解析一律走 `CommonCliOptions::parse()` + `toProcessingOptions()`，不另起炉灶。
   - 参数校验抛 `std::invalid_argument`（配置类）或 `fq::error::ConfigurationError`；库层错误抛 `fq::error::IOError/FormatError`。退出码映射（2/3/4/1）由 `main.cpp` 边界统一完成，命令内不 `return` 差异码。
   - 需要输出目标（报告/新文件）时，复用 `validateReportTargets()` 与「stdout 只允许一个目的地」约定，在跑流水线前 fail-fast。
3. **注册 + CMake**：两处各一行，别忘 `src/cli/CMakeLists.txt`。
4. **测试四层**（e2e 约定：可执行文件路径取 `FASTQTOOLS` 环境变量，默认 `build/clang-release/FastQTools`；样本用 `scripts/datagen/gen_fastq.py` 现生成；临时目录收敛到 `tests/data/tmp/`）。
5. **文档三处**：`docs/cli-reference.md` 加小节；README 入口表/用法提及；对新手有价值的加 getting-started。参数默认值必须与代码一致。
6. **CHANGELOG**：`[Unreleased]` 下按 Added/Fixed/Changed 归类，一句话讲行为而不是实现。
7. **决策笔记判断**：命令引入了新行为、跨文件契约或流程约定 → 按 `.agents/skills/write-notes-like-deepseek` 写 `.agents/notes/`（如 `implemented/feature/yyyy-mm-dd-<name>-command.md`）；纯机械新增免写。
8. **质量门**：`./scripts/core/lint format` → `./scripts/core/test` → 合并前 `./scripts/core/lint all` + sanitizer 本地跑（与 CI 等价命令见 `ci.yml` 头部注释）。

## 硬约束（来自 AGENTS.md，违反即返工）

- CLI 产物链路不引入新依赖（nlohmann_json 已被否，见 `.agents/notes/rejected/architecture/2026-08-18-nlohmann-json-for-stat-cli.md`）。
- 不静默吞异常；日志用 `fq::logging`，换行统一 `"\n"`。
- 新公共 API 需有真实调用方或测试覆盖，不为假设扩展预留。
- commit message 中文，Conventional Commits：`feat(cli): 新增 <name> 命令`。

## 完成检查清单

- [ ] registry 注册 + CMake 源列表
- [ ] unit / integration / e2e 至少各有一条用例覆盖新命令
- [ ] `docs/cli-reference.md`、`README.md` 同步
- [ ] `CHANGELOG.md` [Unreleased] 有条目
- [ ] 非平凡决策已写 `.agents/notes/`（或确认免写）
- [ ] `lint format` + `./scripts/core/test` 全绿
