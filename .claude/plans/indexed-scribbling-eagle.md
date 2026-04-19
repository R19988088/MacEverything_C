# P0 + P1 修复：WAL 就地回放 + 内容索引增量扫描

## Context

从运行日志观察到两个性能瓶颈：
- **P0**: WAL 回放 O(N_total) — 对 9K~44K 条 WAL 条目的回放，因 `exportRecords()`→pathMap→remove_if→`loadRecords()` 全量操作 4.7M 记录，耗时 10~28s。
- **P1**: 内容索引全量扫描 — `startContentIndexing()` 遍历全部 ~4.7M 记录只为找到 ~139 个已索引文件，耗时 ~2s。

两者的根因都是「已有优化基础设施但未被利用」：
- SearchEngine 已有 `pathIndex_`（在 `loadRecords()` 后已构建），可直接用于 O(1) 查找而非重建 pathMap。
- ContentIndex 已有 `fileInfos_`（~139 entries）但无公开方法获取其键列表。

---

## P0: WAL 就地回放

### 问题
`IndexPersistence::load()` L63-119 的 WAL 回放流程：
1. `engine_->exportRecords()` — 拷贝全部 4.7M 记录（O(N_total)）
2. 构建 `pathMap` unordered_map — O(N_total)
3. 应用 WAL 条目 — O(N_wal)
4. `std::remove_if` 删除 tombstone — O(N_total)
5. `engine_->loadRecords()` 重建全部辅助结构 — O(N_total)

**关键发现**：`loadRecords()` 在步骤 1 之前已被 `PagedIndexWriter::load()` 调用，此时 `pathIndex_`、`lowerNames_`、`pathIndices_`、trigram 索引等已全部构建完毕。WAL 回放完全无需重建它们。

### 修复方案

在 `SearchEngine` 新增 `replayWALEntries(std::vector<WALEntry>&& entries)` 方法，直接利用已有的 `pathIndex_` 做就地变更：

```cpp
// SearchEngine.h — public 声明
void replayWALEntries(std::vector<WALEntry>&& entries);
```

```cpp
// SearchEngine.cpp — 实现
void SearchEngine::replayWALEntries(std::vector<WALEntry>&& entries) {
    std::unique_lock lock(mutex_);
    for (auto& entry : entries) {
        std::string lowerPath = me::toLower(entry.fullPath);
        switch (entry.op) {
            case WALOp::Add:
            case WALOp::Update: {
                // 如果已有该路径，先 tombstone 旧记录
                auto it = pathIndex_.find(lowerPath);
                if (it != pathIndex_.end()) {
                    uint32_t oldIdx = it->second;
                    removeTrigramsForRecord(oldIdx);
                    records_[oldIdx].type = 0;
                    markPageDirty(oldIdx);
                    records_[oldIdx].name.clear();
                    records_[oldIdx].size = 0;
                    records_[oldIdx].modTime = 0;
                    lowerNames_[oldIdx].clear();
                    pathIndex_.erase(it);
                    liveCount_.fetch_sub(1, std::memory_order_relaxed);
                }
                // 追加新记录
                uint32_t newIdx = static_cast<uint32_t>(records_.size());
                std::string newFullPath = makeFullPath(entry.record.path, entry.record.name);
                std::string lower = me::toLower(entry.record.name);
                uint32_t pIdx = pathTable_.intern(entry.record.path);
                entry.record.path.clear();
                entry.record.path.shrink_to_fit();
                records_.push_back(std::move(entry.record));
                lowerNames_.push_back(lower);
                pathIndices_.push_back(pIdx);
                pathIndex_[me::toLower(newFullPath)] = newIdx;
                addTrigramsForRecord(newIdx, lower);
                if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
                    dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
                }
                markPageDirty(newIdx);
                liveCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            case WALOp::Remove: {
                auto it = pathIndex_.find(lowerPath);
                if (it != pathIndex_.end()) {
                    uint32_t idx = it->second;
                    removeTrigramsForRecord(idx);
                    records_[idx].type = 0;
                    markPageDirty(idx);
                    records_[idx].name.clear();
                    records_[idx].size = 0;
                    records_[idx].modTime = 0;
                    lowerNames_[idx].clear();
                    pathIndex_.erase(it);
                    liveCount_.fetch_sub(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    }
    rebuildRecentCache();
}
```

**与现有 `addRecord`/`removeByPath`/`updateByPath` 的区别**：
- 不写 WAL（回放本身就是在读 WAL）
- 批量处理后只调用一次 `rebuildRecentCache()`
- 在已持有 exclusive lock 下执行，无需每条单独加锁

### IndexPersistence::load() 修改

替换 L63-119 的 WAL 回放块：

```cpp
// Before: exportRecords + pathMap + 逐条处理 + remove_if + loadRecords
// After:
auto entries = IndexWAL::readAll(walPath_);
if (!entries.empty()) {
    LOG_INFO("IndexPersistence", "Replaying " << entries.size()
              << " WAL entries (in-place mode)");
    engine_->replayWALEntries(std::move(entries));
    LOG_INFO("IndexPersistence", "WAL replay done, live records="
              << engine_->liveRecordCount());
}
```

### 涉及文件

| 文件 | 变更 |
|------|------|
| `MacEverything/Core/SearchEngine.h` | 新增 `replayWALEntries()` 声明 |
| `MacEverything/Core/SearchEngine.cpp` | 新增 `replayWALEntries()` 实现 |
| `MacEverything/Core/IndexPersistence.cpp` | 替换 L63-119 WAL 回放块 |

---

## P1: 内容索引增量扫描

### 问题
`startContentIndexing()` 遍历全部 ~4.7M 记录（`allIndices [0..total)`），调用 `forEachRecordWithPath` 逐个筛选 `type==1` 的文件，然后传给 `indexFile()`。实际上只有 ~139 个文件需要检查。

### 修复方案

#### 1. ContentIndex 新增 `getIndexedFileIndices()`

```cpp
// ContentIndex.h — public 声明
std::vector<uint32_t> getIndexedFileIndices() const;

// ContentIndex.cpp — 实现
std::vector<uint32_t> ContentIndex::getIndexedFileIndices() const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    result.reserve(fileInfos_.size());
    for (const auto& [idx, _] : fileInfos_) {
        result.push_back(idx);
    }
    return result;
}
```

#### 2. 修改 `startContentIndexing()`

将全量扫描替换为两阶段：
- **阶段 A**：对已索引文件（~139 个）做 modTime 检查和重新索引
- **阶段 B**：仅在无已索引文件时（首次运行或 rebuild）执行全量扫描

```cpp
void ServiceEngine::startContentIndexing() {
    // ... 保留前置检查不变 ...

    dispatch_group_async(backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        auto contentStart = std::chrono::steady_clock::now();

        // 获取已索引文件的 indices
        auto existingIndices = contentIndex->getIndexedFileIndices();

        struct FileEntry { uint32_t idx; std::string fullPath; time_t modTime; };
        auto fileEntries = std::make_shared<std::vector<FileEntry>>();

        if (!existingIndices.empty()) {
            // 增量模式：只检查已索引的文件
            fileEntries->reserve(existingIndices.size());
            engine->forEachRecordWithPath(existingIndices,
                [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                    if (r.type != 1) return;
                    fileEntries->push_back({idx,
                        SearchEngine::makeFullPath(path, r.name), r.modTime});
                });
        } else {
            // 全量模式（首次运行或 rebuild 后）：扫描所有文件
            uint32_t total = engine->recordCount();
            std::vector<uint32_t> allIndices;
            allIndices.reserve(total);
            for (uint32_t i = 0; i < total; i++) allIndices.push_back(i);
            fileEntries->reserve(total);
            engine->forEachRecordWithPath(allIndices,
                [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                    if (r.type != 1) return;
                    fileEntries->push_back({idx,
                        SearchEngine::makeFullPath(path, r.name), r.modTime});
                });
        }

        // ... 后续 dispatch_apply 逻辑保持不变 ...
    });
}
```

### 涉及文件

| 文件 | 变更 |
|------|------|
| `MacEverything/Core/ContentIndex.h` | 新增 `getIndexedFileIndices()` 声明 |
| `MacEverything/Core/ContentIndex.cpp` | 新增 `getIndexedFileIndices()` 实现 |
| `MacEverything/Core/ServiceEngine+Content.cpp` | 修改 `startContentIndexing()` 为两阶段 |

---

## 测试

### P0 测试: `tests/test_wal_inplace_replay.h`

| Test | 内容 |
|------|------|
| 1 | `replayWALEntries` Add 操作：记录正确添加，pathIndex_ 可查到 |
| 2 | `replayWALEntries` Remove 操作：记录被 tombstone，pathIndex_ 中移除 |
| 3 | `replayWALEntries` Update 操作：旧记录 tombstone + 新记录追加 |
| 4 | 混合操作：Add + Update + Remove 序列后状态正确 |
| 5 | 空 WAL 条目列表：不崩溃，状态不变 |
| 6 | 不写 WAL 验证：回放后 WAL 未被写入（wal_ 为 null 时） |

### P1 测试: 复用 `tests/test_content_modtime.h`

已有测试覆盖 modTime 跳过逻辑。新增：

| Test | 内容 |
|------|------|
| 1 | `getIndexedFileIndices()` 返回正确的键集合 |
| 2 | 空 contentIndex 时返回空 vector |

---

## 验证

1. `make test_all && ./test_all --fast` 全部通过
2. `xcodebuild` 构建通过
3. 启动 app → 日志验证：
   - P0: WAL 回放日志显示 "in-place mode"，耗时从 10~28s 降至 <1s
   - P1: Content indexing 耗时从 ~2s 降至 <0.1s（增量模式）
4. 搜索功能正常工作（验证 trigram 索引、pathIndex_ 完整性）

## 文件总览

| 类型 | 文件 |
|------|------|
| **修改** | `SearchEngine.h`, `SearchEngine.cpp`, `IndexPersistence.cpp` |
| **修改** | `ContentIndex.h`, `ContentIndex.cpp`, `ServiceEngine+Content.cpp` |
| **新增测试** | `tests/test_wal_inplace_replay.h` |
| **修改测试** | `tests/test_content_modtime.h` |
| **新增文档** | `docs/changelog/` 下变更文档 |
