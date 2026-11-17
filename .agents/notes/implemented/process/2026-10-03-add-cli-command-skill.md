# Agent Note: 新增 CLI 命令流程沉淀为项目 skill（add-cli-command）

Status: implemented

## Problem

新增一个 CLI 子命令不是单文件改动：command 类（`src/cli/commands/`）、`createDefaultCommandRegistry` 注册、`fq_cli` CMake 源列表、unit/integration/e2e 四层测试、`docs/cli-reference.md` 与 README、CHANGELOG `[Unreleased]`，共约 10 个触点。此前新增命令靠 AGENTS.md 的笼统约束加临场翻 `stat`/`filter` 两个现存实现拼流程，漏一处（高频坑：CMake 源列表、e2e 用例、cli-reference 默认值与代码失同步）要等测试或 review 报绿才发现。

## Decision

新增项目级 skill `.agents/skills/add-cli-command/SKILL.md`（约 70 行，符合 write-a-skill 的结构与描述规范），固化三块内容：触点全景表、8 步流程（动手前查 notes → 命令契约照抄 `filter_command.cpp` → 注册与 CMake → 测试四层 → 文档三处 → CHANGELOG → 决策笔记判断 → 质量门）、完成检查清单。skill 描述带触发词（新增/修改 FastQTools 子命令、评审 `src/cli/commands` 改动完整性），按需加载，不占每会话上下文。

skill 内容全部来自对现存实现的实证：`command_interface.h` 的 execute/getName/getDescription 契约、退出码由 `main.cpp` 边界统一映射、e2e 的 `FASTQTOOLS` 环境变量与 `scripts/datagen/gen_fastq.py` 样本约定、临时目录收敛到 `tests/data/tmp/`。两条既有否决记录作为动手前置约束内链其中：产品面收窄（[full-qc-scope](../../rejected/feature/2026-07-18-full-qc-scope.md)，「补齐 fastp 功能面」式命令先确认立项）与 CLI 产物链路不引新依赖（[nlohmann-json-for-stat-cli](../../rejected/architecture/2026-08-18-nlohmann-json-for-stat-cli.md)）。

关联笔记当场审计：现有 5 篇笔记（2 篇 rejected、2 篇 simplification、1 篇 feature rejected）无流程类重叠，`implemented/process/` 为首篇；本篇不取代任何旧篇。

## Alternatives considered

- **只在 AGENTS.md 加一节「新增命令清单」** — 最强论据：零新文件，AGENTS.md 每会话必载入、无需触发判断。被否：AGENTS.md 是全员规范速查，塞入十余行操作级清单会稀释密度；且操作清单需要完整触点表与示例，放 skill 里可按需加载完整版。
- **不沉淀，继续照抄现存命令** — 最强论据：`stat`/`filter` 就是活样例，加文档违反 YAGNI。被否：样例只展示「长什么样」，不展示「要动哪几处」；每个触点漏项的发现成本（测试报绿 → 定位 → 补）累计已超过维护一篇 skill 的成本。

## Consequences

- **收益**：触点漏项从「靠 review 兜底」变为「清单机械核对」；流程知识脱离对话记忆与个体经验，任何 Agent 拿到命令需求都能对表执行。
- **代价与同步义务**：skill 记录的是当前结构事实（`createDefaultCommandRegistry` 装配、e2e 目录约定、fq_cli 目标名）——命令装配方式或测试布局变化时必须**原地同步**该 skill，与决策笔记同规；决定翻转（如注册机制重构）才另立笔记互链。
- **登记边界**：本 skill 为本地编写，不进 `skills-lock.json`（该文件只锁外部 GitHub 来源 skill 的哈希），无需登记。

## Verification

`./scripts/core/notes verify` 通过（本篇与结构校验同批落盘）；`rg -n "registerCommand" src/cli/command_registry.cpp` 的注册方式与 skill 触点表一致；`tests/e2e/test_cli.sh` 头部注释的 `FASTQTOOLS` 默认路径与 skill 描述一致。
