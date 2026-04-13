#pragma once
// ─────────── Helpers ───────────

static size_t getMemoryUsageMB() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size / (1024 * 1024);
    }
    return 0;
}

static int passed = 0, failed = 0;

static void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "    [PASS] " << msg << "\n";
        passed++;
    } else {
        std::cout << "    [FAIL] " << msg << "\n";
        failed++;
    }
}
