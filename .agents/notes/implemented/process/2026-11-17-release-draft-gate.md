# Agent Note: Release 门禁与 draft 人工发布

Status: implemented

## Problem

`release.yml` 在 `v*` tag 上直接创建或更新 Release，没有校验 tag 与 CMake
`project(... VERSION ...)`、CHANGELOG 是否一致；`gh release upload --clobber` 还会
覆盖已存在的正式 Release。x86_64、aarch64 与测试 job 也会在元数据错误时继续消耗
构建时间。

## Decision

- 新增 `validate-release` job：要求 tag 为 `vMAJOR.MINOR.PATCH`，版本号与
  `CMakeLists.txt` 的 `project(... VERSION ...)` 一致，CHANGELOG 存在同名且非空的
  `## [版本]` 段。
- 通过 GitHub API 查询同名 Release：不存在或仍是 draft 才继续；已正式发布则失败。
- 三个构建/测试 job 均依赖 `validate-release`，元数据错误在昂贵构建前终止。
- `publish` 只创建 draft，或更新已存在的 draft；正式 Release 一律拒绝修改。
- 已发布 Release 的资产只能通过新版本替代，不允许重跑旧 tag 覆盖。

## Alternatives considered

- **继续在 publish 中 `--clobber`** — 被否：会把正式发布资产变成可变对象，重跑
  workflow 可能静默替换用户已经下载的版本。
- **只校验 tag 前缀 `v`** — 被否：无法发现版本号、CMake 项目和 CHANGELOG 三者漂移。
- **让构建 job 与校验并行以节省时间** — 被否：发布频率低，错误 tag 触发两套原生
  构建和完整测试的成本更高；先校验再构建更符合发布门禁语义。

## Consequences

- 发布流程从“tag 即上线”变为“tag 生成 draft，人工核对后发布”；多一步维护动作，
  换取版本、CHANGELOG 与二进制资产的一致性。
- 当前仓库尚未创建 tag；首次发布时需先准备对应的 CHANGELOG 版本段。
- draft Release 不会自动成为 latest；只有维护者手动发布后才对外可见。

## Verification

- `release.yml` PyYAML 解析通过；shellcheck（替换 GitHub 表达式后）通过；
  `git diff --check` 通过。
- 用 workflow 原始 shell 片段和 `gh` 模拟器覆盖：合法 tag、CMake 版本不匹配、
  非法 tag、CHANGELOG 空段、Release 不存在、draft、已发布、API 错误，以及
  publish 的创建/更新/拒绝分支。
- 未创建真实 tag 或 Release；仅做静态与本地模拟验证。
