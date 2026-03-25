// Integration test: full daemon lifecycle
// Tests:
//   1. Start Coordinator, DaemonClient health_check returns {status: ok}
//   2. DaemonClient get_status returns response with "state" field
//   3. DaemonClient stop, coordinator exits within 2s
//   4. After stop, socket file is cleaned up
//   5. File watcher: create .py file, wait, get_status shows files_indexed >= 1

#include "daemon/coordinator.h"
#include "daemon/daemon_client.h"
#include "daemon/status.h"
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Create a temp project dir with .codetldr/ subdirectory and a sample .py file.
// Returns the project root path.
static fs::path create_temp_project(const std::string& tag) {
    fs::path root = fs::temp_directory_path() / ("codetldr_lifecycle_" + tag);
    fs::remove_all(root);
    fs::create_directories(root / ".codetldr");

    // Write a sample Python source file
    std::ofstream f(root / "sample.py");
    f << "def hello():\n    return 'Hello, world!'\n\n"
      << "class Greeter:\n    def greet(self, name):\n        return f'Hi {name}'\n";
    f.close();
    return root;
}

// Open a Database at the standard location inside project_root/.codetldr/
static codetldr::Database open_project_db(const fs::path& project_root) {
    return codetldr::Database::open(project_root / ".codetldr" / "index.sqlite");
}

// Run coordinator.run() in a std::thread; returns the thread.
// The caller must join/detach the thread after stopping the coordinator.
static std::thread run_coordinator_in_thread(codetldr::Coordinator& coord) {
    return std::thread([&coord]() {
        try {
            coord.run();
        } catch (const std::exception& ex) {
            std::cerr << "[coordinator thread] exception: " << ex.what() << "\n";
        } catch (...) {
            std::cerr << "[coordinator thread] unknown exception\n";
        }
    });
}

// Wait up to max_ms for the socket file to appear.
static bool wait_for_socket(const fs::path& sock_path, int max_ms = 3000) {
    for (int i = 0; i < max_ms / 50; ++i) {
        if (fs::exists(sock_path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Make a single RPC call via a FRESH DaemonClient connection.
// The IpcServer handles one request per accepted connection — this is the correct
// way to send multiple RPCs: create a new DaemonClient for each call.
// Returns the full response JSON. Throws on connection failure.
static nlohmann::json rpc_call(const fs::path& sock_path,
                               const std::string& method,
                               const nlohmann::json& params = nullptr) {
    codetldr::DaemonClient client;
    if (!client.connect(sock_path)) {
        throw std::runtime_error("rpc_call: could not connect to " + sock_path.string());
    }
    return client.call(method, params);
}

// Send stop request and wait for coordinator thread to finish (with timeout).
static bool stop_coordinator(const fs::path& sock_path, std::thread& thread) {
    try {
        rpc_call(sock_path, "stop");
    } catch (...) {
        // Ignore errors — coordinator may have already started shutting down
    }
    auto future = std::async(std::launch::async, [&thread]() {
        if (thread.joinable()) thread.join();
    });
    return future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}

// -----------------------------------------------------------------------
// Test 1: health_check
// -----------------------------------------------------------------------
static void test_1_health_check() {
    std::cout << "Test 1: health_check returns {status: ok}..." << std::flush;

    fs::path root = create_temp_project("t1");
    fs::path sock_path = root / ".codetldr" / "daemon.sock";

    auto db = open_project_db(root);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(root, db.raw(), registry, sock_path,
                                std::chrono::seconds(30));
    auto thread = run_coordinator_in_thread(coord);

    // Wait for socket — each IPC request uses a fresh DaemonClient connection
    bool appeared = wait_for_socket(sock_path);
    assert(appeared && "Test 1: socket should appear within 3s");

    auto resp = rpc_call(sock_path, "health_check");
    assert(resp.contains("result") && "Test 1: response should have result");
    assert(resp["result"].value("status", "") == "ok" &&
           "Test 1: result.status should be 'ok'");
    assert(resp["result"].value("pid", 0) > 0 &&
           "Test 1: result.pid should be > 0");

    bool stopped = stop_coordinator(sock_path, thread);
    assert(stopped && "Test 1: coordinator should stop within 5s");
    fs::remove_all(root);

    std::cout << " PASS\n";
}

// -----------------------------------------------------------------------
// Test 2: get_status has state field
// -----------------------------------------------------------------------
static void test_2_get_status() {
    std::cout << "Test 2: get_status returns state field..." << std::flush;

    fs::path root = create_temp_project("t2");
    fs::path sock_path = root / ".codetldr" / "daemon.sock";

    auto db = open_project_db(root);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(root, db.raw(), registry, sock_path,
                                std::chrono::seconds(30));
    auto thread = run_coordinator_in_thread(coord);

    bool appeared = wait_for_socket(sock_path);
    assert(appeared && "Test 2: socket should appear");

    auto resp = rpc_call(sock_path, "get_status");
    assert(resp.contains("result") && "Test 2: response should have result");
    assert(resp["result"].contains("state") && "Test 2: result should have 'state' field");

    bool stopped = stop_coordinator(sock_path, thread);
    assert(stopped && "Test 2: coordinator should stop within 5s");
    fs::remove_all(root);

    std::cout << " PASS\n";
}

// -----------------------------------------------------------------------
// Test 3: stop causes coordinator to exit within 2s
// -----------------------------------------------------------------------
static void test_3_stop_and_exit() {
    std::cout << "Test 3: stop causes coordinator to exit within 2s..." << std::flush;

    fs::path root = create_temp_project("t3");
    fs::path sock_path = root / ".codetldr" / "daemon.sock";

    auto db = open_project_db(root);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(root, db.raw(), registry, sock_path,
                                std::chrono::seconds(30));
    auto thread = run_coordinator_in_thread(coord);

    bool appeared = wait_for_socket(sock_path);
    assert(appeared && "Test 3: socket should appear");

    // Send stop — expect {ok: true} in response
    auto stop_resp = rpc_call(sock_path, "stop");
    assert(stop_resp.contains("result") && "Test 3: stop should return result");
    assert(stop_resp["result"].value("ok", false) && "Test 3: stop result.ok should be true");

    // Wait for thread to finish within 2s
    auto future = std::async(std::launch::async, [&thread]() {
        if (thread.joinable()) thread.join();
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    assert(status == std::future_status::ready &&
           "Test 3: coordinator should exit within 2s after stop");

    fs::remove_all(root);
    std::cout << " PASS\n";
}

// -----------------------------------------------------------------------
// Test 4: after stop, socket file is cleaned up
// -----------------------------------------------------------------------
static void test_4_socket_cleanup() {
    std::cout << "Test 4: socket file cleaned up after stop..." << std::flush;

    fs::path root = create_temp_project("t4");
    fs::path sock_path = root / ".codetldr" / "daemon.sock";

    auto db = open_project_db(root);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(root, db.raw(), registry, sock_path,
                                std::chrono::seconds(30));
    auto thread = run_coordinator_in_thread(coord);

    bool appeared = wait_for_socket(sock_path);
    assert(appeared && "Test 4: socket should appear");
    assert(fs::exists(sock_path) && "Test 4: socket file exists before stop");

    // Stop coordinator
    try { rpc_call(sock_path, "stop"); } catch (...) {}

    // Wait for coordinator to exit
    if (thread.joinable()) thread.join();

    // Socket should be cleaned up by coordinator shutdown
    assert(!fs::exists(sock_path) && "Test 4: socket file should not exist after stop");

    fs::remove_all(root);
    std::cout << " PASS\n";
}

// -----------------------------------------------------------------------
// Test 5: File watch integration — create .py file, get_status shows indexed > 0
// -----------------------------------------------------------------------
static void test_5_file_watch_integration() {
    std::cout << "Test 5: file watch triggers indexing..." << std::flush;

    // Use a fresh project with no pre-existing Python files
    fs::path root = fs::temp_directory_path() / "codetldr_lifecycle_t5";
    fs::remove_all(root);
    fs::create_directories(root / ".codetldr");

    fs::path sock_path = root / ".codetldr" / "daemon.sock";

    auto db = open_project_db(root);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(root, db.raw(), registry, sock_path,
                                std::chrono::seconds(30));
    auto thread = run_coordinator_in_thread(coord);

    bool appeared = wait_for_socket(sock_path);
    assert(appeared && "Test 5: socket should appear");

    // Wait 500ms for file watcher to start watching
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Write a new .py file into the watched directory
    {
        std::ofstream f(root / "new_file.py");
        f << "def add(a, b):\n    return a + b\n";
    }

    // Wait 3s for debounce (2s) + analysis cycle
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Query status via fresh connection
    auto resp = rpc_call(sock_path, "get_status");
    assert(resp.contains("result") && "Test 5: get_status should have result");
    int files_indexed = resp["result"].value("files_indexed", 0);
    assert(files_indexed >= 1 && "Test 5: files_indexed should be >= 1 after file creation");

    bool stopped = stop_coordinator(sock_path, thread);
    assert(stopped && "Test 5: coordinator should stop");
    fs::remove_all(root);

    std::cout << " PASS\n";
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main() {
    // Ignore SIGPIPE: occurs when coordinator closes server-side socket
    // and client tries to write on the same (now-half-closed) connection.
    ::signal(SIGPIPE, SIG_IGN);

    std::cout << "=== Daemon Lifecycle Integration Tests ===\n";

    try {
        test_1_health_check();
        test_2_get_status();
        test_3_stop_and_exit();
        test_4_socket_cleanup();
        test_5_file_watch_integration();

        std::cout << "\nAll 5 tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\nFATAL: unknown exception\n";
        return 1;
    }
}
