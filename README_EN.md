<p align="center">
  <img src="MacEverything/Assets.xcassets/AppIcon.appiconset/icon_256.png" alt="MacEverything" width="128" />
</p>

<h1 align="center">MacEverything</h1>

<p align="center">
  <b>Instant file search for macOS</b> — find any file among millions in milliseconds.<br/>
  Inspired by <a href="https://www.voidtools.com/">Everything</a> on Windows. Nothing else comes close on Mac.
</p>

<p align="center">
  <a href="README.md">中文</a> | <b>English</b>
</p>

<p align="center">
  <a href="#installation"><img src="https://img.shields.io/badge/macOS-13%2B-blue?logo=apple" alt="macOS 13+" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License" /></a>
  <a href="#testing"><img src="https://img.shields.io/badge/tests-77%20modules-brightgreen" alt="77 test modules" /></a>
  <a href="#mcp-integration"><img src="https://img.shields.io/badge/MCP-compatible-blueviolet" alt="MCP Compatible" /></a>
</p>

---

<p align="center">
  <img src="assets/screen-shot.jpg" alt="MacEverything Screenshot" width="720" />
</p>

## Why MacEverything?

Spotlight is slow. `find` is slower. `mdfind` misses files. MacEverything indexes your **entire disk — 5 million+ files — in seconds**, then answers every search in **under 5ms**.

| What | MacEverything | Spotlight | `find` |
|------|:---:|:---:|:---:|
| Index 5M files | ~14s | Minutes+ | N/A (no index) |
| Search latency (typical) | **< 5ms** | 200ms–2s | 5–30s |
| Real-time FS monitoring | FSEvents | FSEvents | None |
| Content search | Trigram index | Metadata-heavy | `grep` |
| AI-tool integration (MCP) | Built-in | No | No |

## Key Features

### Blazing Fast Search

- **Trigram inverted index** — sub-linear search, 33x–300x faster than brute-force linear scan
- **ARM NEON SIMD** string matching — 11.5 GB/s single-thread, 74 GB/s multi-thread (60% of memory bandwidth on M3 Pro)
- **GCD parallel query** — leverages all CPU cores for linear-scan fallback paths
- **< 5ms average** for trigram-accelerated queries across 5.4M indexed files ([benchmarks →](#benchmarks))

### Full-Text Content Search

- `infile:keyword` — trigram-indexed full-text search with highlighted context snippets
- Incremental updates via FNV-1a content hashing — only re-indexes changed files
- Configurable file types and size limits

### Rich Query Language

Everything-style query syntax with full AST parser:

```
*.swift                      # glob patterns
ext:py size:>1mb             # filters
readme path:/usr             # path-scoped search
"exact phrase" OR alternative   # boolean operators
dm:today type:folder         # date & type filters
case:Makefile                # case-sensitive mode
infile:TODO                  # content search
```

### Real-Time File System Monitoring

- **FSEvents** file-level event listener — index stays current without polling
- **WAL (Write-Ahead Log)** with CRC32 checksums — crash-safe persistence
- Startup replays from last `eventId` — no full rebuild, ever

### MCP Integration

Built-in [Model Context Protocol](https://modelcontextprotocol.io/) server — lets AI coding tools search your filesystem instantly.

```
Claude Code / Cursor / Claude Desktop
       │
       ▼  (stdio JSON-RPC 2.0)
  MacEverythingMCP
       │
       ▼  (HTTP localhost:19860)
  MacEverything.app
```

**4 MCP Tools:**

| Tool | Description |
|------|-------------|
| `search_files` | Filename search (trigram-accelerated) |
| `search_content` | Full-text content search |
| `recent_files` | Recently modified files |
| `index_status` | Index statistics and health |

One-click setup for **Claude Code**, **Cursor**, and **Claude Desktop** — toggle from the app's menu bar.

### HTTP API

Local REST API on `localhost:19860` for scripting and automation:

```bash
# Search files
curl "http://localhost:19860/api/search?q=readme&limit=10"

# Content search
curl "http://localhost:19860/api/search/content?q=TODO"

# Recent files
curl "http://localhost:19860/api/recent?limit=20"

# Index status
curl "http://localhost:19860/api/status"
```

Every search response includes detailed `timing` telemetry (totalMs, trigramMs, candidates, searchPath, etc.).

## Benchmarks

Tested on macOS Darwin 24.3.0 with **5.4 million indexed files**, 48 query types, 26 rounds of iterative optimization:

### Search Latency

| Query Type | Avg Latency | Example |
|------------|:-----------:|---------|
| Long keywords (7+ chars) | **0.1–1ms** | `screenshot` 0.1ms, `dockerfile` 0.1ms |
| Medium keywords (4–6 chars) | **1–5ms** | `readme` 1.2ms, `config` 4.7ms |
| Glob patterns | **0.7–18ms** | `*.cpp` 0.7ms, `*.swift` 1.5ms |
| Path queries | **3–32ms** | `package.json` 2.9ms |
| All 48 queries (avg) | **39.8ms** | Includes edge cases & linear fallbacks |

### Trigram vs Linear Scan

| Query | Trigram | Linear | Speedup |
|-------|:------:|:------:|:-------:|
| `node_modules` | 0.5ms | 154ms | **308x** |
| `application` | 2.1ms | 175ms | **83x** |
| `readme` | 1.2ms | 49ms | **41x** |

### SIMD String Search (Apple M3 Pro)

| Method | Throughput | vs `std::string::find` |
|--------|:---------:|:----------------------:|
| `std::string::find` | 1.2 GB/s | baseline |
| **NEON 128-bit (1 thread)** | **11.5 GB/s** | **9.5x** |
| **NEON 128-bit (12 threads)** | **74.3 GB/s** | **60.7x** |

## Architecture

```
┌─────────────────────────────────────┐
│       SwiftUI App Layer             │  UI · ViewModel · MVVM
├─────────────────────────────────────┤
│    Objective-C++ Bridge Layer       │  Zero-overhead interop
├─────────────────────────────────────┤
│       C++20 Core Engine             │  Scan · Index · Search · Persist
└─────────────────────────────────────┘
```

### Core Engine Highlights

| Component | Key Design |
|-----------|-----------|
| **DirectoryScanner** | Multi-threaded work-stealing via `getattrlistbulk` — single syscall for bulk attribute reads |
| **SearchEngine** | Trigram inverted index + SoA columnar layout + `__builtin_prefetch` for cache-friendly access |
| **ContentIndex** | Trigram-based inverted index with FNV-1a content hashing for incremental updates |
| **SIMDSearch** | ARM NEON 128-bit vectorized string matching with 2x loop unrolling |
| **IndexPersistence** | WAL + CRC32 + paged dirty-page flushing with atomic rename |
| **FileSystemWatcher** | FSEvents monitoring with incremental eventId replay |
| **PathTable** | String interning — directory paths stored as `uint32` index, not full strings |
| **QueryParser** | Full AST pipeline: Tokenizer → FilterParser → Parser → QueryAST |

### Deployment Modes

| Mode | Description |
|------|-------------|
| **GUI App** | SwiftUI menu-bar app, `Option+Space` global hotkey |
| **CLI Daemon** | Headless `maceverything-daemon` — same engine, no UI |
| **MCP Server** | `MacEverythingMCP` — stdio JSON-RPC proxy for AI tools |

## Installation

### From DMG (Recommended)

1. Download `MacEverything.dmg` from [Releases](../../releases)
2. Drag `MacEverything.app` to Applications
3. Launch and grant **Full Disk Access** when prompted
4. Wait for initial scan (~14s for a typical disk)
5. Press `Option+Space` to search

### Build from Source

**Requirements:** macOS 13+, Xcode 15+

```bash
# Clone
git clone https://github.com/user/MacEverything.git
cd MacEverything

# Build
xcodebuild -project MacEverything.xcodeproj -scheme MacEverything \
  -configuration Release build SYMROOT=build

# Package DMG
hdiutil create -volname MacEverything \
  -srcfolder build/Release/MacEverything.app \
  -ov -format UDZO MacEverything.dmg
```

### CLI Daemon

```bash
make daemon
./maceverything-daemon --port 19860 --root /
```

## Testing

77 test modules covering the full stack — from trigram index correctness to MCP protocol compliance.

```bash
make test          # Fast unit tests + bridge lint
make test-slow     # Integration tests (full disk scan, FSEvents, E2E)
make test-all      # Everything
make test-asan     # AddressSanitizer
make test-tsan     # ThreadSanitizer
```

Test categories include:
- **Core engine**: scan, query, mutation, compaction, ranking, path search
- **Persistence**: WAL CRC integrity, batch replay, race conditions, paged persistence
- **Content index**: trigram, compaction, mod-time tracking, WAL tracking
- **Search/Query**: tokenizer, parser, filters, date filters, structured queries, regex trigram
- **Performance**: SIMD search, 10M-record synthetic benchmarks, trigram competition
- **Integration**: thread safety, E2E, HTTP engine swap, MCP protocol (29 assertions)
- **Sanitizers**: ASan + TSan builds for memory and threading safety

## Usage

1. **Grant Full Disk Access** — the app guides you through this on first launch
2. **Wait for initial scan** — progress shows in the menu bar (~14s typical)
3. **`Option+Space`** — summon the search window from anywhere
4. **Type to search** — results appear instantly as you type
5. **`infile:keyword`** — switch to full-text content search
6. **Right-click** — open, reveal in Finder, copy path, drag & drop

### Query Examples

| Query | What it finds |
|-------|--------------|
| `readme` | All files containing "readme" |
| `*.swift` | All Swift source files |
| `ext:py size:>1mb` | Large Python files |
| `dm:today` | Files modified today |
| `config path:/usr` | "config" files under `/usr` |
| `infile:TODO ext:cpp` | C++ files containing "TODO" |
| `type:folder node_modules` | Directories named "node_modules" |

## Project Structure

```
MacEverything/
├── Core/                  # C++20 core engine
│   ├── SearchEngine       # Trigram index + parallel query (5 .cpp files)
│   ├── DirectoryScanner   # Multi-threaded bulk scanner
│   ├── ContentIndex       # Full-text inverted index
│   ├── IndexPersistence   # WAL + paged persistence
│   ├── FileSystemWatcher  # FSEvents real-time monitor
│   ├── HttpServer         # Embedded REST API server
│   ├── SIMDSearch         # ARM NEON vectorized search
│   ├── QueryAST/Parser    # Full query language pipeline
│   ├── PathTable          # String interning table
│   └── ServiceEngine      # Lifecycle orchestration
├── Bridge/                # Objective-C++ bridge
│   └── MacSearchBridge    # C++ ↔ Swift zero-overhead interop
├── App/                   # SwiftUI application
│   ├── ContentView        # Main search UI
│   ├── SearchViewModel    # MVVM with tiered debouncing
│   ├── HotkeyManager      # Global hotkey registration
│   └── MCPConfigManager   # One-click MCP setup
├── CLI/                   # Command-line tools
│   ├── daemon_main        # Headless daemon
│   └── mcp_main           # MCP server (stdio JSON-RPC)
└── tests/                 # 77 test modules
```

## Performance Design

| Technique | Impact |
|-----------|--------|
| `getattrlistbulk` | Single syscall for bulk file attributes — avoids per-file `stat` |
| Trigram inverted index | Sub-linear search: 33x–300x faster than linear scan |
| SoA columnar layout | Cache-friendly memory access patterns for filtering |
| `__builtin_prefetch` | Hides random memory access latency during candidate verification |
| ARM NEON SIMD | 128-bit vectorized string matching, 9.5x single-thread speedup |
| GCD parallel scan | Multi-core linear scan when trigram can't help |
| PathTable interning | Directory paths stored as `uint32` — massive memory savings |
| Generation counter | Auto-cancels stale queries every 1024 iterations during fast typing |
| APFS Firmlink dedup | Correctly handles macOS Data/System volume merge loops |
| Thread-local bitmap | O(1) trigram deduplication during content extraction (2^24 bits) |
| Adaptive trigram bypass | Falls back to parallel scan when trigram selectivity is poor |
| 80ms/300ms tiered debounce | Balances responsiveness with resource usage in the UI |

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`feat/...`) or bugfix branch (`fix/...`)
3. Write tests for new functionality
4. Ensure `make test-all` passes
5. Submit a pull request

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

<p align="center">
  <b>If MacEverything helps you find files faster, consider giving it a star!</b>
</p>
