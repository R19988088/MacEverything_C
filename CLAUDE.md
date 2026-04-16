# CLAUDE.md

## 项目结构

- `MacEverything/Core/` — C++20 核心引擎（扫盘、搜索、持久化）
- `MacEverything/Bridge/` — Objective-C++ 桥接层
- `MacEverything/App/` — SwiftUI 界面层
- `requirements.md` — 产品需求文档

## 构建

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -project MacEverything.xcodeproj -scheme MacEverything -configuration Release build SYMROOT=build
```

## 打包
总是在master分支上打包

```bash
hdiutil create -volname MacEverything -srcfolder build/Release/MacEverything.app -ov -format UDZO /Users/wujian/data/project/mac_everything/MacEverything.dmg
```


# 软件开发工作流（Agent 必须遵守）

## 0. 版本管理（Git）

- 所有代码变更必须通过 Git 管理；禁止在未纳入版本控制的路径上长期开发。
- 保持历史可读：原子提交、信息明确的 commit message（建议说明「做什么」与「为何」）。
- `master`（或项目约定的主分支）保持稳定、可发布；合入前须通过约定检查（测试、lint 等）。
- **任何变更/对文件的修改都应该创建worktree, 开发完成后需要提交,确保没有任何未提交修改**

## 1. 变更/功能开发与 Bugfix 流程

按顺序执行，不得跳过「规划」与「测试设计」直接大改代码。

1. **规划**：明确范围、接口/行为变更、风险与回滚方式；复杂任务拆成可验证的小步。对于bug修复, 生成plan后进行review, 确认是彻底解决了问题,还是在打补丁
2. **测试先行**：在实现前写出或补全**可失败**的测试（单元或更上层），用例应描述期望行为与边界；实现应使这些测试通过。
3. **隔离开发（worktree）**：**所有变更**都在独立 `git worktree` 中开发，避免污染当前检出目录；分支命名清晰（如 `feat/...`、`fix/...`）。
4. **合回主分支**：开发完成后经 code review（若流程要求）、CI/本地测试通过后，合并回 `master`（优先 merge 或按项目约定 rebase），删除已完成任务的 worktree 与远程分支。 开发完成后**一定要提交变更,合并回master分支**
5. 对于新的功能,可以根据需要,启动subagent并行开发, 提升开发效率
6. 每次完成后, 输出一个md文档到docs/changelog/里面,名字使用"{数字}-xxxx"格式, 数字根据docs/changelog/下最大的文件递增, 内容为详细描述这次变更的规划和实施情况等等, commit

Agent 在接到功能或 bug 任务时，应先输出简短计划与测试清单，再进入实现。

## 2. 集成测试

- 在模块边界、真实 I/O（数据库、HTTP、消息队列等按项目适用）处维护**集成测试**，覆盖主路径与关键错误路径。
- 新功能或行为变更应同步增加或更新集成测试，避免仅依赖单元测试或手工验证。
- 集成测试应可重复、可在 CI 中运行（或使用 testcontainers 等可复现环境）。

## 3. Bugfix：根因优先，禁止「贴膏药」

- **先确认现象**：稳定复现步骤、影响范围、是否与版本/环境相关。
- **再分析本质**：用调试、日志、最小复现定位根因（设计缺陷、错误假设、状态不一致、并发等），写清「为什么错」再动代码。
- **系统性修复**：在正确抽象层修复；调整数据模型、API 契约或不变量，使同类错误难以再发生。
- **禁止**：仅为了压症状而加 `try/catch` 吞异常、无文档的魔法数/特判、复制粘贴重复逻辑、在未理解根因时堆叠防御性分支。

若信息不足以判断根因，应先补充观测（日志、测试、最小复现），而不是先打补丁。

## 4. 编码规范
**禁止出现超大文件**, 如果出现1000行以上的文件, 应该进行功能拆分和重构, 方便agent更好的理解文件

## 5. 测试组织规范
- 独立的测试应放在 `tests/` 目录下的单独头文件中（如 `tests/test_xxx.h`），并在 `test_all.cpp` 中通过 `#include` 引入
- **禁止**在 `test_all.cpp` 中直接实现测试函数，`test_all.cpp` 仅负责 include 测试模块、CLI 参数解析和 `main()` 调度


---

## 快速自检（Agent 收尾前）

- [ ] 变更在 Git 中，且 worktree/分支策略符合第 1 节  
- [ ] 新行为有对应测试；关键路径有集成测试覆盖  
- [ ] Bugfix 能说明根因与修复为何不会反复出现  
- [ ] 没有任何未提交修改, 代码已经合并到master分支
- [ ] 变更文档是否已经生成, 是否commit