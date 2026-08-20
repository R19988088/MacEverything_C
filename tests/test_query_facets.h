#pragma once

#include "SearchEngine.h"
#include "FileCategory.h"

static void runQueryFacetTests() {
    FileCategoryCounts counts;
    me::file_category::accumulate(counts, 1, "asset-note.txt");
    me::file_category::accumulate(counts, 2, "asset-folder");
    me::file_category::accumulate(counts, 1, "asset-photo.jpg");
    me::file_category::accumulate(counts, 1, "asset-movie.mov");
    me::file_category::accumulate(counts, 1, "asset-song.flac");
    me::file_category::accumulate(counts, 1, "asset-pack.7z");
    me::file_category::accumulate(counts, 3, "asset-link.mp3");
    me::file_category::accumulate(counts, 5, "asset-tool.app");

    check(counts.files == 7, "77.1 files excludes only directory");
    check(counts.applications == 1, "77.2 application count");
    check(counts.folders == 1, "77.3 folder count");
    check(counts.images == 1, "77.4 image count");
    check(counts.videos == 1, "77.5 video count");
    check(counts.audio == 2, "77.6 audio includes matching symlink");
    check(counts.archives == 1, "77.7 archive count");
    check(me::file_category::matches(FileCategory::Applications, 5, "asset-tool.app"),
          "77.8 app bundle is an application");
    check(!me::file_category::matches(FileCategory::Applications, 2, "asset-folder.app"),
          "77.9 ordinary directory is not an application");
    check(me::file_category::matches(FileCategory::Files, 5, "asset-tool.app"),
          "77.10 app bundle remains a file");
    check(!me::file_category::matches(FileCategory::Files, 2, "asset-folder"),
          "77.11 directory is not a file");
}

static void runQueryFacetQueryTests() {
    SearchEngine engine;
    engine.addRecord(FileRecord{"asset-note.txt", "/fixture", 1, 10, 1});
    engine.addRecord(FileRecord{"asset-folder", "/fixture", 2, 0, 1});
    engine.addRecord(FileRecord{"asset-photo.JPG", "/fixture", 1, 20, 1});
    engine.addRecord(FileRecord{"asset-movie.mov", "/fixture", 1, 30, 1});
    engine.addRecord(FileRecord{"asset-song.flac", "/fixture", 1, 40, 1});
    engine.addRecord(FileRecord{"asset-pack.7z", "/fixture", 1, 50, 1});
    engine.addRecord(FileRecord{"asset-link.mp3", "/fixture", 3, 0, 1});
    engine.addRecord(FileRecord{"asset-tool.app", "/fixture", 5, 0, 1});

    const auto capped = engine.queryFaceted("asset", 1, true, 7701);
    check(capped.counts.files == 7, "77.12 payload cap preserves exact file count");
    check(capped.counts.applications == 1, "77.13 payload cap preserves exact application count");
    check(capped.indicesFor(FileCategory::Applications).size() == 1,
          "77.14 application payload is independently capped");
    check(capped.indicesFor(FileCategory::Files).size() == 1,
          "77.15 file payload is capped");
    check(capped.indicesFor(FileCategory::Folders).size() == 1,
          "77.16 folder payload is independently capped");

    const auto full = engine.queryFaceted("asset", 10000, true, 7702);
    check(full.indicesFor(FileCategory::Files).size() == 7,
          "77.17 all non-folders cached");
    check(full.indicesFor(FileCategory::Applications).size() == 1,
          "77.18 only app bundles are cached as applications");
    check(full.indicesFor(FileCategory::Images).size() == 1,
          "77.19 image payload");
    check(full.counts.images == 1,
          "77.20 uppercase extension is classified through lowercase namePool");
    check(full.indicesFor(FileCategory::Files)[0] != full.indicesFor(FileCategory::Folders)[0],
          "77.21 category payloads remain separate");

    engine.cancelSession(7703);
    const auto cancelled = engine.queryFaceted("asset", 10000, true, 7703);
    check(cancelled.counts.files == 7,
          "77.22 a new query after cancellation receives a fresh generation");
}
