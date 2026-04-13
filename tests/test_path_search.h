#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3b: Path-based Search Tests
// ═══════════════════════════════════════════════════════

static void runPathSearchTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3b: Path-based Search Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;

    std::vector<FileRecord> records;
    records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
    records.push_back({"config.json", "/etc/myapp", 1, 200, 2000});
    records.push_back({"readme.md", "/home/user/projects", 1, 300, 3000});
    records.push_back({"libfoo.dylib", "/usr/local/lib", 1, 400, 4000});
    records.push_back({"include", "/usr/local", 2, 0, 5000});
    engine.loadRecords(std::move(records));

    // Search by path substring
    auto res = engine.query("/usr/local");
    check(res.size() == 3, "Path search '/usr/local': 3 matches (src/main.cpp, lib/libfoo.dylib, include)");

    res = engine.query("/etc");
    check(res.size() == 1, "Path search '/etc': 1 match");
    check(engine.getRecord(res[0]).name == "config.json", "Path search '/etc': correct file");

    // Search by directory name only
    res = engine.query("projects");
    check(res.size() == 1, "Dir name search 'projects': 1 match");
    check(engine.getRecord(res[0]).name == "readme.md", "Dir name search 'projects': correct file");

    // Search that matches both name and path
    res = engine.query("local");
    check(res.size() == 3, "Search 'local': 3 matches (path contains '/usr/local')");

    // Glob on path
    res = engine.query("*/lib/*");
    check(res.size() == 1, "Glob '*/lib/*': 1 match");
    check(engine.getRecord(res[0]).name == "libfoo.dylib", "Glob '*/lib/*': correct file");

    // Path search after addRecord
    engine.addRecord({"newlib.a", "/usr/local/lib", 1, 500, 6000});
    res = engine.query("/usr/local/lib");
    check(res.size() == 2, "After addRecord: '/usr/local/lib' returns 2 matches");

    // Path search after removeByPath
    engine.removeByPath("/usr/local/lib/libfoo.dylib");
    res = engine.query("/usr/local/lib");
    check(res.size() == 1, "After removeByPath: '/usr/local/lib' returns 1 match");

    // Path search after updateByPath
    engine.updateByPath("/usr/local/lib/newlib.a", {"newlib_v2.a", "/opt/lib", 1, 600, 7000});
    res = engine.query("/usr/local/lib");
    check(res.size() == 0, "After updateByPath: '/usr/local/lib' returns 0 matches");
    res = engine.query("/opt/lib");
    check(res.size() == 1, "After updateByPath: '/opt/lib' returns 1 match");

    std::cout << "\n";
}
