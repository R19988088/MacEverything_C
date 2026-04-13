# CLAUDE.md

## 开发工作流规范

对于每次新增需求，严格按以下步骤执行：
1. **计划** — 分析需求，设计实现方案
2. **实现** — 编码实现功能
3. **测试** — 更新测试用例
4. **文档** — 更新需求文档 (requirements.md)

## 分支与测试规范

修复问题或性能优化前，必须遵循以下流程：
1. **新建分支** — 从 master 创建专用分支（如 `fix/xxx` 或 `perf/xxx`）
2. **编写测试** — 先生成测试用例，验证问题确实存在或度量当前性能基线
3. **实施修复** — 在分支上进行代码修改
4. **验证测试** — 运行测试，确认修复后测试符合预期
5. **合并回 master** — 测试通过后，merge 回 master 分支

## 评审分析规范

当用户要求对项目进行评审或分析时：
1. **启动 subagent** — 使用 Agent 工具启动子代理，对整个项目进行全面分析
2. **性能问题** — 检查 I/O 瓶颈、内存使用、锁竞争、不必要的拷贝等
3. **系统 Bug** — 检查边界条件、资源泄漏、线程安全、崩溃风险等
4. **输出报告** — 汇总发现的问题，按严重程度排序

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

```bash
hdiutil create -volname MacEverything -srcfolder build/Release/MacEverything.app -ov -format UDZO /Users/wujian/data/project/mac_everything/MacEverything.dmg
```
