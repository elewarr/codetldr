// Test: Daemon IPC server, request router, daemon client, status writer
// Tests:
//   1. IpcServer binds to temp socket, DaemonClient connects and gets health_check response
//   2. Stale socket detection: file exists but no listener -> removed, bind succeeds
//   3. Live server: second bind_or_die returns false (refuses to clobber)
//   4. Unknown method returns JSON-RPC error {code: -32601}
//   5. Stop request returns {ok: true}
//   6. StatusWriter write/read roundtrip
//   7. bind_or_die rejects paths exceeding sun_path limit
//   8. DaemonClient::connect rejects paths exceeding sun_path limit

#include "daemon/ipc_server.h"
#include "daemon/daemon_client.h"
#include "daemon/request_router.h"
#include "daemon/coordinator.h"
#include "daemon/status.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace fs = std::filesystem;

// Helper: generate a unique temp socket path
static fs::path make_temp_sock_path(const std::string& name) {
    return fs::temp_directory_path() / ("codetldr_test_" + name + ".sock");
}

// Helper: generate a unique temp directory
static fs::path make_temp_dir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("codetldr_test_dir_" + name);
    fs::create_directories(p);
    return p;
}

// ============================================================
// Test 1: IpcServer bind + DaemonClient connect + health_check
// ============================================================
static void test_1_health_check() {
    std::cout << "Test 1: health_check roundtrip..." << std::flush;

    fs::path sock = make_temp_sock_path("t1");
    // Cleanup if leftover
    fs::remove(sock);

    // We need a minimal Coordinator-like server. Since the real server needs
    // a Database & LanguageRegistry, we test the wire protocol directly:
    // Run the server in a thread using IpcServer and inline dispatch.

    codetldr::IpcServer server;
    bool bound = server.bind_or_die(sock);
    assert(bound && "Test 1: bind_or_die should succeed");

    std::thread server_thread([&server]() {
        // Use poll to wait for connection
        struct pollfd pfd{};
        pfd.fd = server.server_fd();
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 2000);  // 2s timeout
        if (rc <= 0) return;

        int client_fd = server.accept_client();
        if (client_fd < 0) return;

        std::string msg = server.recv_message(client_fd);
        nlohmann::json req = nlohmann::json::parse(msg);

        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"]      = req.value("id", 1);
        if (req.value("method", "") == "health_check") {
            response["result"]["status"] = "ok";
            response["result"]["pid"]    = static_cast<int>(::getpid());
        }
        server.send_message(client_fd, response);
        ::close(client_fd);
    });

    // Give server thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    codetldr::DaemonClient client;
    bool connected = client.connect(sock);
    assert(connected && "Test 1: DaemonClient should connect");

    nlohmann::json resp = client.call("health_check");
    server_thread.join();

    assert(resp.contains("result") && "Test 1: response should have result");
    assert(resp["result"].value("status", "") == "ok" && "Test 1: status should be ok");
    assert(resp["result"].contains("pid") && "Test 1: response should have pid");

    server.cleanup();
    fs::remove(sock);
    std::cout << " PASS\n";
}

// ============================================================
// Test 2: Stale socket cleanup
// ============================================================
static void test_2_stale_socket() {
    std::cout << "Test 2: stale socket cleanup..." << std::flush;

    fs::path sock = make_temp_sock_path("t2");
    fs::remove(sock);

    // Create a socket file but don't bind/listen on it (simulates stale socket).
    // We create the file manually so there's no listener.
    {
        // Bind and close immediately to leave stale file
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        assert(fd >= 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
        ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        ::close(fd);  // Close without listen — will cause ECONNREFUSED on connect
    }

    assert(fs::exists(sock) && "Test 2: stale socket file should exist");

    codetldr::IpcServer server;
    bool bound = server.bind_or_die(sock);
    assert(bound && "Test 2: bind_or_die should succeed after removing stale socket");
    assert(server.server_fd() >= 0 && "Test 2: server_fd should be valid");

    server.cleanup();
    fs::remove(sock);
    std::cout << " PASS\n";
}

// ============================================================
// Test 3: Live server — second bind_or_die returns false
// ============================================================
static void test_3_live_server() {
    std::cout << "Test 3: refuse to clobber live daemon..." << std::flush;

    fs::path sock = make_temp_sock_path("t3");
    fs::remove(sock);

    codetldr::IpcServer server1;
    bool bound1 = server1.bind_or_die(sock);
    assert(bound1 && "Test 3: first server should bind");

    codetldr::IpcServer server2;
    bool bound2 = server2.bind_or_die(sock);
    assert(!bound2 && "Test 3: second bind_or_die should return false for live socket");

    server1.cleanup();
    fs::remove(sock);
    std::cout << " PASS\n";
}

// ============================================================
// Test 4: Unknown method -> JSON-RPC error -32601
// We test RequestRouter::dispatch directly using a mock Coordinator
// ============================================================

// Minimal stub coordinator for testing request routing without a real DB
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"
#include <cstdlib>

static void test_4_unknown_method() {
    std::cout << "Test 4: unknown method returns -32601..." << std::flush;

    // Build a full Coordinator to test RequestRouter
    // Use a temp dir as project root
    fs::path tmp_dir = make_temp_dir("t4");
    fs::path codetldr_dir = tmp_dir / ".codetldr";
    fs::create_directories(codetldr_dir);
    fs::path db_path = codetldr_dir / "index.sqlite";
    fs::path sock_path = codetldr_dir / "daemon.sock";

    auto db = codetldr::Database::open(db_path);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(tmp_dir, db.raw(), registry, sock_path);

    // Access router through coordinator via a direct dispatch test
    // We test the RequestRouter directly
    codetldr::RequestRouter router(coord, db.raw());

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = "nonexistent_method";

    nlohmann::json resp = router.dispatch(req);

    assert(resp.contains("error") && "Test 4: response should have error field");
    assert(resp["error"].value("code", 0) == -32601 &&
           "Test 4: error code should be -32601");

    fs::remove_all(tmp_dir);
    std::cout << " PASS\n";
}

// ============================================================
// Test 5: Stop request -> {ok: true}
// ============================================================
static void test_5_stop_request() {
    std::cout << "Test 5: stop request returns {ok: true}..." << std::flush;

    fs::path tmp_dir = make_temp_dir("t5");
    fs::path codetldr_dir = tmp_dir / ".codetldr";
    fs::create_directories(codetldr_dir);
    fs::path db_path   = codetldr_dir / "index.sqlite";
    fs::path sock_path = codetldr_dir / "daemon.sock";

    auto db = codetldr::Database::open(db_path);
    codetldr::LanguageRegistry registry;
    registry.initialize();

    codetldr::Coordinator coord(tmp_dir, db.raw(), registry, sock_path);
    codetldr::RequestRouter router(coord, db.raw());

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 2;
    req["method"]  = "stop";

    nlohmann::json resp = router.dispatch(req);

    assert(resp.contains("result") && "Test 5: response should have result");
    assert(resp["result"].value("ok", false) == true &&
           "Test 5: result.ok should be true");

    fs::remove_all(tmp_dir);
    std::cout << " PASS\n";
}

// ============================================================
// Test 6: StatusWriter write/read roundtrip
// ============================================================
static void test_6_status_writer() {
    std::cout << "Test 6: StatusWriter write/read roundtrip..." << std::flush;

    fs::path tmp_dir = make_temp_dir("t6");
    fs::path status_file = tmp_dir / "status.json";

    codetldr::StatusWriter writer(status_file);

    codetldr::DaemonStatus original;
    original.state          = codetldr::DaemonState::kIdle;
    original.pid            = static_cast<pid_t>(42);
    original.socket_path    = "/tmp/test/daemon.sock";
    original.files_indexed  = 100;
    original.files_total    = 200;
    original.last_indexed_at = "2026-03-25T12:00:00Z";
    original.uptime_seconds = 3600;

    writer.write(original);

    assert(fs::exists(status_file) && "Test 6: status.json should be created");

    codetldr::DaemonStatus readback = codetldr::StatusWriter::read(status_file);

    assert(readback.state == codetldr::DaemonState::kIdle && "Test 6: state mismatch");
    assert(readback.pid == 42 && "Test 6: pid mismatch");
    assert(readback.socket_path == "/tmp/test/daemon.sock" &&
           "Test 6: socket_path mismatch");
    assert(readback.files_indexed == 100 && "Test 6: files_indexed mismatch");
    assert(readback.files_total   == 200 && "Test 6: files_total mismatch");
    assert(readback.last_indexed_at == "2026-03-25T12:00:00Z" &&
           "Test 6: last_indexed_at mismatch");
    assert(readback.uptime_seconds == 3600 && "Test 6: uptime_seconds mismatch");

    fs::remove_all(tmp_dir);
    std::cout << " PASS\n";
}

// ============================================================
// Test 7: bind_or_die rejects paths exceeding sun_path limit
// ============================================================
static void test_7_server_overlong_path() {
    std::cout << "Test 7: bind_or_die rejects overlong socket path..." << std::flush;

    codetldr::IpcServer server;
    // Generate a path longer than sizeof(sockaddr_un::sun_path) — use 200 chars
    std::string long_path(200, 'x');
    fs::path overlong_sock = fs::temp_directory_path() / long_path;
    bool caught = false;
    try {
        server.bind_or_die(overlong_sock);
    } catch (const std::runtime_error& e) {
        caught = true;
        assert(std::string(e.what()).find("Socket path too long") != std::string::npos);
    }
    assert(caught && "Expected runtime_error for overlong socket path");
    std::cout << " PASS\n";
}

// ============================================================
// Test 8: DaemonClient::connect rejects overlong socket path
// ============================================================
static void test_8_client_overlong_path() {
    std::cout << "Test 8: DaemonClient::connect rejects overlong socket path..." << std::flush;

    codetldr::DaemonClient client;
    std::string long_path(200, 'x');
    fs::path overlong_sock = fs::temp_directory_path() / long_path;
    bool caught = false;
    try {
        client.connect(overlong_sock);
    } catch (const std::runtime_error& e) {
        caught = true;
        assert(std::string(e.what()).find("Socket path too long") != std::string::npos);
    }
    assert(caught && "Expected runtime_error for overlong socket path");
    std::cout << " PASS\n";
}

int main() {
    std::cout << "=== Daemon IPC Tests ===\n";

    try {
        test_1_health_check();
        test_2_stale_socket();
        test_3_live_server();
        test_4_unknown_method();
        test_5_stop_request();
        test_6_status_writer();
        test_7_server_overlong_path();
        test_8_client_overlong_path();

        std::cout << "\nAll 8 tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        return 1;
    }
}
