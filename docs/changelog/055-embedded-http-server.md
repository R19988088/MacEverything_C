# 055 - Embedded HTTP Server

## Summary

Added an embedded HTTP API server to MacEverything, enabling programmatic access to the search engine via localhost HTTP requests. The server listens on `127.0.0.1:19860` and provides REST-style JSON APIs for filename search, content search, recent files, engine status, and health checks.

## Motivation

External tools, scripts, and browser extensions can benefit from querying the MacEverything index without going through the GUI. An HTTP API makes the index accessible to any HTTP client (curl, Python, Alfred workflows, etc.) while keeping the server local-only for security.

## Changes

### New Files

- **`MacEverything/Core/HttpServer.h`** (~55 lines): Header declaring the `HttpServer` class with POSIX socket-based accept loop, HTTP request parsing, and route dispatch.
- **`MacEverything/Core/HttpServer.cpp`** (~340 lines): Full implementation including:
  - POSIX socket lifecycle (`socket` / `bind` / `listen` / `accept` / `close`)
  - `poll()`-based accept loop with 500ms timeout for clean shutdown via `running_` flag
  - HTTP request parsing (method, path, query string with URL decoding)
  - JSON response generation with proper string escaping
  - CORS header (`Access-Control-Allow-Origin: *`) for browser-based clients

### Modified Files

- **`MacEverything/Bridge/MacSearchBridge_Internal.h`**: Added `#include "HttpServer.h"` and `std::shared_ptr<HttpServer> _httpServer` ivar.
- **`MacEverything/Bridge/MacSearchBridge.h`**: Added `startHttpServer:` and `stopHttpServer` method declarations.
- **`MacEverything/Bridge/MacSearchBridge.mm`**: Implemented `startHttpServer:` / `stopHttpServer` bridge methods; added `stopHttpServer` call at the beginning of `prepareForTermination`.
- **`MacEverything/App/AppDelegate.swift`**: Added `MacSearchBridge.shared().startHttpServer(19860)` in `applicationDidFinishLaunching`.
- **`MacEverything.xcodeproj/project.pbxproj`**: Registered HttpServer.h and HttpServer.cpp in the build.

## API Endpoints

| Endpoint | Params | Description |
|---|---|---|
| `GET /api/search?q=&limit=` | q (required), limit (default 100, max 10000) | Filename substring search |
| `GET /api/search/content?q=&limit=` | q (required), limit (default 100, max 10000) | Full-text content search |
| `GET /api/recent?limit=` | limit (default 100, max 10000) | Recently modified files |
| `GET /api/status` | none | Record counts and content index stats |
| `GET /api/health` | none | Health check (returns `{"status":"ok"}`) |

## Design Decisions

1. **Localhost-only binding** (`127.0.0.1`): No network exposure; only local processes can connect.
2. **No third-party dependencies**: Hand-rolled HTTP parsing and JSON serialization using POSIX sockets and `std::ostringstream`.
3. **Synchronous per-connection handling**: Acceptable for a local-only API with low concurrency. Each connection is handled sequentially in the accept thread.
4. **Clean shutdown**: The accept loop uses `poll()` with a 500ms timeout to check the `running_` flag, ensuring the server stops promptly when the app terminates.
5. **Port 19860**: Chosen as an uncommon port unlikely to conflict with other local services.

## Testing

- Build verification: project compiles successfully with the new files.
- Manual testing: After app launch, `curl http://127.0.0.1:19860/api/health` should return `{"status":"ok"}`.
