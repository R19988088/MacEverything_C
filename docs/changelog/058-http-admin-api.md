# 058 — HTTP Admin API Extensions

## Summary

Extended the embedded HTTP server with management API endpoints, enabling automation tools and scripts to control index rebuilding and content index configuration via HTTP.

## New Endpoints

| Endpoint | Method | Description | Response |
|---|---|---|---|
| `POST /api/index/rebuild` | POST | Triggers file name index rebuild (async) | 202 Accepted |
| `POST /api/content/rebuild` | POST | Triggers content index rebuild (async) | 202 Accepted |
| `GET /api/content/config` | GET | Returns current content index config | 200 OK |
| `POST /api/content/config` | POST | Updates content index config (partial) | 200 OK |

### Request/Response Examples

```bash
# Get content config
curl http://127.0.0.1:19860/api/content/config
# {"extensions":["h","cpp","swift","py"],"maxFileSize":1048576}

# Update config (supports partial updates)
curl -X POST -H "Content-Type: application/json" \
  -d '{"extensions":["h","cpp","swift"],"maxFileSize":2097152}' \
  http://127.0.0.1:19860/api/content/config

# Trigger rebuilds
curl -X POST http://127.0.0.1:19860/api/index/rebuild
curl -X POST http://127.0.0.1:19860/api/content/rebuild
```

## Architecture

### AdminCallbacks Pattern

The HTTP server is pure C++ and cannot directly call ObjC++ Bridge methods. The solution uses `std::function` callbacks:

1. `HttpServer::AdminCallbacks` — a struct of 5 `std::function` callbacks declared in `HttpServer.h`
2. `HttpServer::setAdminCallbacks()` — setter called by the Bridge after `start()`
3. Bridge injects closures that call the appropriate management operations:
   - `onRebuildIndex` → posts `NSNotification @"rebuildIndex"` (handled by SearchViewModel)
   - `onRebuildContentIndex` → dispatches `[self rebuildContentIndex]` on utility queue
   - `onSetContentConfig` → calls `ContentIndex::setExtensions()` / `setMaxFileSize()` via thread-safe accessor
   - `onGetContentExtensions` / `onGetContentMaxFileSize` → returns current config via thread-safe accessor

### POST Support

Added HTTP POST body reading:
- `handleConnection()` now parses `Content-Length` header and reads additional data if the body was split across TCP segments
- `parseRequest()` extracts the body (everything after `\r\n\r\n`)
- `route()` now dispatches both GET and POST methods to appropriate handlers
- Simple JSON body parser for the fixed `{"extensions":[...],"maxFileSize":N}` structure

## Files Changed

| File | Change |
|---|---|
| `MacEverything/Core/HttpServer.h` | Added `AdminCallbacks` struct, `setAdminCallbacks()`, `body` field in `HttpRequest`, 4 new handler declarations |
| `MacEverything/Core/HttpServer.cpp` | POST body reading, body extraction in parser, dual GET/POST routing, 4 new admin handlers with JSON parsing, 202 status text |
| `MacEverything/Bridge/MacSearchBridge.mm` | `startHttpServer:` now injects `AdminCallbacks` closures after `start()` |

## Testing

```bash
# Build verification
xcodebuild -scheme MacEverything -configuration Release build

# Manual API tests (with app running):
curl http://127.0.0.1:19860/api/health
curl http://127.0.0.1:19860/api/content/config
curl -X POST http://127.0.0.1:19860/api/content/config -d '{"maxFileSize":2097152}'
curl -X POST http://127.0.0.1:19860/api/content/rebuild
curl -X POST http://127.0.0.1:19860/api/index/rebuild
```
