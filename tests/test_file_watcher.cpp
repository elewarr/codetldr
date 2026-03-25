#include "watcher/file_watcher.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace codetldr;
using namespace std::chrono_literals;

namespace fs = std::filesystem;

// Test 6: create a temp dir, start watching, write a file, verify event is received via queue within 3s
void test_file_watcher_event_received() {
    // Create a temp directory
    fs::path tmp_dir = fs::temp_directory_path() / ("test_file_watcher_" + std::to_string(::getpid()));
    fs::create_directories(tmp_dir);

    std::atomic<bool> event_received{false};
    std::string received_path;
    std::atomic<bool> wakeup_called{false};

    auto on_event = [&](std::string path) {
        received_path = path;
        event_received.store(true, std::memory_order_relaxed);
    };
    auto on_wakeup = [&]() {
        wakeup_called.store(true, std::memory_order_relaxed);
    };

    FileWatcher watcher(on_event, on_wakeup);
    watcher.start(tmp_dir);

    // Give watcher a moment to initialize
    std::this_thread::sleep_for(200ms);

    // Write a test file
    fs::path test_file = tmp_dir / "test.cpp";
    {
        std::ofstream f(test_file);
        f << "int main() { return 0; }\n";
    }

    // Wait up to 3s for the event
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!event_received.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    watcher.stop();

    // Cleanup
    try { fs::remove_all(tmp_dir); } catch (...) {}

    assert(event_received.load() && "FileWatcher: event should be received within 3s");
    // received_path should contain "test.cpp" somewhere
    assert(received_path.find("test.cpp") != std::string::npos &&
           "Received path should contain the test filename");
    // on_wakeup should have been called
    assert(wakeup_called.load() && "on_wakeup should have been called");

    std::cout << "PASS: test_file_watcher_event_received (path=" << received_path << ")\n";
}

int main() {
    test_file_watcher_event_received();
    std::cout << "All file_watcher tests passed.\n";
    return 0;
}
