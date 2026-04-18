# 090 - lowerPathPool_ 预计算小写路径 + 去重路径匹配

## 背景

对 4.7M 记录进行 2 字符查询（如 `"py"`）需要线性扫描，耗时 ~164ms。瓶颈在于：对每条记录都需要从 `pathPool_` 读取原始大小写路径，通过 `simdToLowerAscii` 转小写后再 `simdContains` 搜索。而 `pathPool_` 是去重的（~100K 条 vs 4.7M 记录），相同路径被重复转小写了数十次。

## 方案

### 1. 新增 `lowerPathPool_` 成员
- `StringPool lowerPathPool_` 与 `pathPool_` 逐条目并行，存储预计算好的小写路径
- 所有搜索路径直接从 `lowerPathPool_` 读取，消除运行时 `simdToLowerAscii` 调用

### 2. 引入 `internPath()` 私有方法
- 统一 6 处重复的 path intern 代码（loadRecords, addRecord, updateByPath, batchRescanPrefix, replayWALEntries x2）
- 原子地向 `pathPool_` 和 `lowerPathPool_` 双写，通过 `pathLookup_` 去重

### 3. 去重路径预匹配
- 线性扫描前预扫描 ~100K 唯一小写路径，结果存入 `vector<bool> pathMatchCache`
- 4.7M 次循环中 O(1) 位查表替代每条记录的 SIMD 路径搜索
- 特殊处理 slash 边界：当关键词含 `/` 时，即使 pathMatchCache 为 false 也需构建全路径验证

## 修改文件

| 文件 | 变更 |
|------|------|
| `MacEverything/Core/SearchEngine.h` | 添加 `lowerPathPool_` 成员、`internPath()` 声明、更新 `buildPathTrigramIndexFromData` 参数名 |
| `MacEverything/Core/SearchEngine.cpp` | internPath() 实现；6 处 mutation site 替换；7 处搜索 site 替换 pathPool_→lowerPathPool_；trigram 构建去掉 toLower；compactRecords COW 并行构建；去重路径预匹配 |
| `test_all.cpp` | 注册 Part 52 测试 |
| `tests/test_path_search.h` | 新增 22 个测试：大小写路径搜索、addRecord/updateByPath 后搜索、compaction 后搜索、slash 边界去重、原始大小写保留 |

## 验证结果

- 全部 10,969 个 fast 测试通过
- Release 构建成功
- HTTP 验证：`q=py` 查询正常返回结果，`q=/users` 路径搜索正确
- `q=config.json` trigram 查询 90ms，功能无退化
