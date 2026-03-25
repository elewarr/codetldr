#include "watcher/debouncer.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

using namespace codetldr;
using namespace std::chrono_literals;

// Test 1: touch("a.cpp") then flush_ready() immediately returns empty (within window)
void test_flush_ready_within_window() {
    Debouncer d(50ms);  // 50ms window
    d.touch("a.cpp");
    auto ready = d.flush_ready();
    assert(ready.empty() && "flush_ready() should return empty within quiet window");
    std::cout << "PASS: test_flush_ready_within_window\n";
}

// Test 2: touch("a.cpp"), wait > window, flush_ready() returns ["a.cpp"]
void test_flush_ready_after_window() {
    Debouncer d(50ms);
    d.touch("a.cpp");
    std::this_thread::sleep_for(70ms);
    auto ready = d.flush_ready();
    assert(ready.size() == 1 && "flush_ready() should return one entry after window");
    assert(ready[0] == "a.cpp" && "flush_ready() should return the touched path");
    std::cout << "PASS: test_flush_ready_after_window\n";
}

// Test 3: touch("a.cpp") twice rapidly, only one entry after flush (dedup)
void test_dedup_rapid_touches() {
    Debouncer d(50ms);
    d.touch("a.cpp");
    d.touch("a.cpp");
    std::this_thread::sleep_for(70ms);
    auto ready = d.flush_ready();
    assert(ready.size() == 1 && "dedup: rapid touches should produce only one entry");
    assert(ready[0] == "a.cpp");
    std::cout << "PASS: test_dedup_rapid_touches\n";
}

// Test 4: touch("a.cpp") then touch("b.py"), both flushed after window
void test_multiple_files_flushed() {
    Debouncer d(50ms);
    d.touch("a.cpp");
    d.touch("b.py");
    std::this_thread::sleep_for(70ms);
    auto ready = d.flush_ready();
    assert(ready.size() == 2 && "two different files should both be flushed");
    bool found_a = std::find(ready.begin(), ready.end(), "a.cpp") != ready.end();
    bool found_b = std::find(ready.begin(), ready.end(), "b.py") != ready.end();
    assert(found_a && found_b && "both paths should be in flush_ready result");
    std::cout << "PASS: test_multiple_files_flushed\n";
}

// Test 5: next_timeout_ms() returns -1 when empty, positive value when pending
void test_next_timeout_ms() {
    Debouncer d(50ms);
    assert(d.next_timeout_ms() == -1 && "empty debouncer should return -1");
    d.touch("a.cpp");
    int timeout = d.next_timeout_ms();
    assert(timeout >= 0 && timeout <= 50 && "pending debouncer should return positive timeout");
    std::cout << "PASS: test_next_timeout_ms\n";
}

int main() {
    test_flush_ready_within_window();
    test_flush_ready_after_window();
    test_dedup_rapid_touches();
    test_multiple_files_flushed();
    test_next_timeout_ms();

    std::cout << "All debouncer tests passed.\n";
    return 0;
}
