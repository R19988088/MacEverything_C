# MacEverything 变更日志

项目从 2026-04-14 开始开发，以下按时间顺序记录所有重要变更。

## 变更索引

| # | 类型 | 标题 | 日期 |
|---|------|------|------|
| [001](001-测试框架模块化拆分.md) | refactor | 测试框架模块化拆分 | 2026-04-14 |
| [002](002-Core层高严重度Bug修复-C1-C5.md) | bugfix | Core 层高严重度 Bug 修复 (C1-C5) | 2026-04-14 |
| [003](003-Core层中严重度问题修复-Phase2.md) | bugfix | Core 层中严重度问题修复 (Phase 2) | 2026-04-14 |
| [004](004-Bridge层问题修复-B1-B6.md) | bugfix | Bridge 层问题修复 (B1-B6) | 2026-04-14 |
| [005](005-App层问题修复-A1-A8.md) | bugfix | App 层问题修复 (A1-A8) | 2026-04-14 |
| [006](006-12项性能优化.md) | performance | 12 项性能优化（Trigram 索引等） | 2026-04-14 |
| [007](007-Bridge层编译错误与lint检查.md) | bugfix | Bridge 层编译错误与 lint 检查 | 2026-04-14 |
| [008](008-启动挂起防御.md) | bugfix | 启动挂起防御 | 2026-04-14 |
| [009](009-大文件拆分与测试重组.md) | refactor | 大文件拆分与测试重组 | 2026-04-14 |
| [010](010-搜索延迟-recentIndices增量缓存.md) | bugfix | 搜索延迟：recentIndices 增量缓存 | 2026-04-14 |
| [011](011-最近文件列表橙色背景.md) | feature | 最近文件列表橙色背景 | 2026-04-14 |
| [012](012-搜索延迟-查询取消修复.md) | bugfix | 搜索延迟：查询取消修复 | 2026-04-14 |
| [013](013-10个CRITICAL与HIGH级Bug修复.md) | bugfix | 10 个 CRITICAL/HIGH 级 Bug 修复 | 2026-04-14 |
| [014](014-CRITICAL与HIGH并发安全与性能修复.md) | bugfix | CRITICAL/HIGH 并发安全与性能修复 | 2026-04-14 |
| [015](015-最近文件标签徽章样式.md) | feature | 最近文件标签徽章样式 | 2026-04-14 |
| [016](016-搜索栏蓝色边框与字号调整.md) | feature | 搜索栏蓝色边框与字号调整 | 2026-04-14 |
| [017](017-CRITICAL与HIGH第二轮修复.md) | bugfix | CRITICAL/HIGH 第二轮修复 | 2026-04-14 |
| [018](018-快速输入压力测试.md) | test | 快速输入压力测试 (Part 19) | 2026-04-14 |
| [019](019-内存优化550MB.md) | performance | 内存优化：节省约 550 MB | 2026-04-14 |
| [020](020-空闲CPU100%修复.md) | bugfix | 空闲 CPU 100% 修复 | 2026-04-14 |
| [021](021-焦点感知索引刷新节流.md) | bugfix | 焦点感知索引刷新节流 | 2026-04-14 |
| [022](022-索引刷新节流状态机.md) | feature | 索引刷新节流状态机 | 2026-04-14 |
| [023](023-非焦点时跳过刷新.md) | bugfix | 非焦点时跳过刷新 | 2026-04-14 |
| [024](024-移除废弃测试代码.md) | refactor | 移除废弃测试代码 (~970 行) | 2026-04-14 |
| [025](025-信号量累积竞态.md) | bugfix | 信号量累积竞态 | 2026-04-14 |
| [026](026-nil字符串崩溃.md) | bugfix | nil 字符串崩溃 | 2026-04-14 |
| [027](027-移除废弃recordAtIndex接口.md) | refactor | 移除废弃 recordAtIndex 接口 | 2026-04-14 |
| [028](028-rebuildIndex重置内容搜索状态.md) | bugfix | rebuildIndex 重置内容搜索状态 | 2026-04-14 |
| [029](029-窗口查找改用标题匹配.md) | bugfix | 窗口查找改用标题匹配 | 2026-04-14 |
| [030](030-提取共享工具函数.md) | refactor | 提取共享工具函数 | 2026-04-14 |
| [031](031-P2级别批量修复.md) | bugfix | P2 级别批量修复 | 2026-04-14 |
| [032](032-PathTable线程安全与WAL互斥锁.md) | bugfix | PathTable 线程安全与 WAL 互斥锁 | 2026-04-15 |
| [033](033-rebuildIndex任务取消与nil检查.md) | bugfix | rebuildIndex 任务取消与 nil 检查 | 2026-04-15 |
| [034](034-焦点变更冷却与safeEngine修复.md) | bugfix | 焦点变更冷却与 safeEngine 修复 | 2026-04-15 |
| [035](035-代码简化与P2批量修复与LazyVStack.md) | bugfix | 代码简化、P2 批量修复与 LazyVStack | 2026-04-15 |
| [036](036-统一日志系统.md) | feature | 统一日志系统 | 2026-04-15 |
| [037](037-文件拖放支持.md) | feature | 文件拖放支持 | 2026-04-15 |
| [038](038-查询与日志与WAL性能优化.md) | performance | 查询/日志/WAL 性能优化 | 2026-04-15 |
| [039](039-Bridge信号量与LazyVStack修复v2.md) | bugfix | Bridge 信号量与 LazyVStack 修复 v2 | 2026-04-15 |
| [040](040-增量trigram更新.md) | performance | 增量 Trigram 更新 | 2026-04-15 |
| [041](041-单实例文件锁与WAL路径修复.md) | bugfix | 单实例文件锁与 WAL 路径修复 | 2026-04-15 |
| [042](042-WAL批量回放优化.md) | performance | WAL 批量回放优化 | 2026-04-15 |
| [043](043-两阶段即时启动.md) | feature | 两阶段即时启动 | 2026-04-15 |
| [044](044-WAL-rename失败链修复.md) | bugfix | WAL rename 失败链修复 | 2026-04-15 |
| [045](045-重扫防抖与节流.md) | bugfix | 重扫防抖与节流 | 2026-04-15 |
| [046](046-Compaction-FSEvents正反馈循环.md) | bugfix | Compaction-FSEvents 正反馈循环 | 2026-04-15 |
| [047](047-Logger刷新缺陷.md) | bugfix | Logger 刷新缺陷 | 2026-04-15 |
| [048](048-搜索栏幽灵文本自动补全.md) | feature | 搜索栏幽灵文本自动补全 | 2026-04-15 |
| [049](049-跳过空压缩.md) | performance | 跳过空压缩（dirty 标志） | 2026-04-15 |
| [050](050-COW无阻塞压缩.md) | performance | COW 无阻塞压缩 | 2026-04-15 |
| [051](051-压缩阈值与F_NOCACHE.md) | bugfix | 压缩阈值与 F_NOCACHE | 2026-04-15 |
| [052](052-分页增量持久化.md) | performance | 分页增量持久化 | 2026-04-15 |

## 统计

| 类型 | 数量 |
|------|------|
| bugfix | 29 |
| performance | 8 |
| feature | 7 |
| refactor | 5 |
| test | 1 |
| **合计** | **52** |

---

| 严重级别 | 数量 |
|----------|------|
| P0-CRITICAL | 7 |
| P1-HIGH | 12 |
| P2-MEDIUM | 10 |
| 非 bugfix (无级别) | 23 |
