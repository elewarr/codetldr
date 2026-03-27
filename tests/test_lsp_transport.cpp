// Test: LspTransport — write queue, pending map, timeouts, notification dispatch
// Tests:
//   1. request_response_roundtrip: spawn cat, send_request, drain, poll_read — message received
//   2. write_queue_ordering: enqueue 3 notifications, verify queue size 3, drain all, size 0
//   3. write_queue_partial_drain: enqueue large notification, drain incrementally completes
//   4. pending_request_map: send_request stores pending; after response arrives, pending clears
//   5. timeout_fires: set_timeout(1s), send_request, wait, check_timeouts — error callback fires
//   6. notification_dispatch: send_notification, cat echoes it back, poll_read fires handler
//   7. multiple_requests_different_ids: send 3 requests get ids 1, 2, 3; pending_count == 3

#include "lsp/lsp_transport.h"

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <poll.h>

namespace {

// Helper: wait for data to be available on fd (blocking up to timeout_ms)
static bool wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}

// Helper: drain write queue fully (poll writable + drain, up to N iterations)
static bool drain_fully(codetldr::LspTransport& t, int max_iters = 100) {
    for (int i = 0; i < max_iters; ++i) {
        if (t.drain_writes()) return true;
        // Brief wait for pipe to drain
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return t.write_queue_size() == 0;
}

// ============================================================
// Test 1: request_response_roundtrip
// ============================================================
static void test_1_request_response_roundtrip() {
    std::cout << "Test 1: request_response_roundtrip..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 1: spawn should succeed");
    assert(t.is_running() && "Test 1: should be running");

    // send_request assigns id=1
    int64_t id = t.send_request("test/method", {{"x", 1}}, nullptr);
    assert(id == 1 && "Test 1: first request id should be 1");
    assert(t.write_queue_size() == 1 && "Test 1: write queue should have 1 entry");

    // Drain to cat
    bool drained = drain_fully(t);
    assert(drained && "Test 1: drain should complete");
    assert(t.write_queue_size() == 0 && "Test 1: queue should be empty after drain");

    // cat echoes the framed request back — poll_read should receive it
    bool readable = wait_readable(t.stdout_fd(), 1000);
    assert(readable && "Test 1: stdout should be readable after cat echo");

    auto messages = t.poll_read();
    assert(!messages.empty() && "Test 1: at least one message should be received");

    // The echoed message is a JSON-RPC request (has "id" and "method" field)
    // It's echoed verbatim — verify it has the expected id and method
    bool found = false;
    for (auto& msg : messages) {
        if (msg.contains("id") && msg["id"] == 1 && msg.contains("method")) {
            found = true;
            assert(msg["method"] == "test/method" && "Test 1: method should match");
        }
    }
    assert(found && "Test 1: echoed message with id=1 should be found");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 2: write_queue_ordering
// ============================================================
static void test_2_write_queue_ordering() {
    std::cout << "Test 2: write_queue_ordering..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 2: spawn should succeed");

    // Enqueue 3 notifications without draining
    t.send_notification("method/a", {{"n", 1}});
    t.send_notification("method/b", {{"n", 2}});
    t.send_notification("method/c", {{"n", 3}});
    assert(t.write_queue_size() == 3 && "Test 2: queue should have 3 entries");

    // Drain all
    bool drained = drain_fully(t);
    assert(drained && "Test 2: all messages should drain");
    assert(t.write_queue_size() == 0 && "Test 2: queue should be empty after drain");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 3: write_queue_partial_drain
// ============================================================
static void test_3_write_queue_partial_drain() {
    std::cout << "Test 3: write_queue_partial_drain..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 3: spawn should succeed");

    // Build a large (100KB) params object
    std::string large_data(100 * 1024, 'X');
    t.send_notification("large/notification", {{"data", large_data}});
    assert(t.write_queue_size() == 1 && "Test 3: queue should have 1 large entry");

    // Drain fully (may take multiple iterations since pipe buffer is ~64KB on most systems)
    bool drained = drain_fully(t, 500);
    assert(drained && "Test 3: large message should fully drain");
    assert(t.write_queue_size() == 0 && "Test 3: queue should be empty after full drain");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 4: pending_request_map
// ============================================================
static void test_4_pending_request_map() {
    std::cout << "Test 4: pending_request_map..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 4: spawn should succeed");

    // Initially no pending requests
    assert(t.pending_count() == 0 && "Test 4: no pending initially");

    // send_request stores entry in pending map
    std::atomic<bool> callback_called{false};
    int64_t id = t.send_request("test", {}, [&](const nlohmann::json&, const nlohmann::json&) {
        callback_called = true;
    });
    (void)id;
    assert(t.pending_count() == 1 && "Test 4: pending count should be 1 after send_request");

    // Drain the request to cat (cat echoes it)
    drain_fully(t);

    // Wait for echo then poll_read
    // cat echoes the request (has "id" + "method") — this is NOT a response (has "method")
    // So the pending entry is NOT removed by poll_read for requests-echoed-back.
    // To properly test response dispatch, we need to construct and feed a proper response.
    // Instead, verify pending_count stays 1 until timeout or manual clear.
    bool readable = wait_readable(t.stdout_fd(), 500);
    (void)readable;
    t.poll_read(); // consume echo — pending should still be 1 (no matching response)
    assert(t.pending_count() == 1 && "Test 4: pending should still be 1 (echo is not a response)");

    // Test that a proper response (with "result" field and matching id) clears pending
    // Construct a JSON-RPC response and write it directly as a framed message
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {{"ok", true}};
    std::string framed = codetldr::LspFraming::encode(response);
    ssize_t written = ::write(t.stdin_fd(), framed.data(), framed.size());
    assert(written == static_cast<ssize_t>(framed.size()) && "Test 4: write response should succeed");

    // poll_read should now dispatch the response callback and clear pending
    bool readable2 = wait_readable(t.stdout_fd(), 500);
    assert(readable2 && "Test 4: response should be readable");
    t.poll_read();
    assert(t.pending_count() == 0 && "Test 4: pending should be 0 after response dispatched");
    assert(callback_called && "Test 4: callback should have been invoked");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 5: timeout_fires
// ============================================================
static void test_5_timeout_fires() {
    std::cout << "Test 5: timeout_fires..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 5: spawn should succeed");

    // Set 1-second timeout
    t.set_timeout(std::chrono::seconds(1));

    std::atomic<bool> error_received{false};
    int error_code = 0;
    std::string error_msg;

    int64_t id = t.send_request("slow/method", {}, [&](const nlohmann::json& result,
                                                         const nlohmann::json& error) {
        (void)result;
        if (!error.is_null() && error.contains("code")) {
            error_code = error["code"].get<int>();
            error_msg = error["message"].get<std::string>();
            error_received = true;
        }
    });
    (void)id;

    assert(t.pending_count() == 1 && "Test 5: pending count should be 1");

    // Do NOT drain or poll_read — let it time out
    // Wait 1.1 seconds
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // check_timeouts should fire the callback
    t.check_timeouts();

    assert(error_received && "Test 5: timeout callback should have fired");
    assert(error_code == -32000 && "Test 5: error code should be -32000");
    assert(error_msg == "Request timed out" && "Test 5: error message should match");
    assert(t.pending_count() == 0 && "Test 5: pending should be cleared after timeout");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 6: notification_dispatch
// ============================================================
static void test_6_notification_dispatch() {
    std::cout << "Test 6: notification_dispatch..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 6: spawn should succeed");

    // Set notification handler
    std::atomic<bool> handler_called{false};
    std::string received_method;
    t.set_notification_handler([&](const std::string& method, const nlohmann::json& params) {
        (void)params;
        received_method = method;
        handler_called = true;
    });

    // Send a notification — cat echoes it back as-is (no "id" field, has "method")
    t.send_notification("textDocument/publishDiagnostics", {{"uri", "file:///test.cpp"}});

    // Drain to cat
    bool drained = drain_fully(t);
    assert(drained && "Test 6: drain should complete");

    // Wait for echo
    bool readable = wait_readable(t.stdout_fd(), 1000);
    assert(readable && "Test 6: stdout should be readable after cat echo");

    // poll_read should dispatch notification handler
    t.poll_read();

    assert(handler_called && "Test 6: notification handler should have been called");
    assert(received_method == "textDocument/publishDiagnostics" &&
           "Test 6: received method should match");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 7: multiple_requests_different_ids
// ============================================================
static void test_7_multiple_requests_different_ids() {
    std::cout << "Test 7: multiple_requests_different_ids..." << std::flush;

    codetldr::LspTransport t;
    int err = t.spawn("/bin/cat", {});
    assert(err == 0 && "Test 7: spawn should succeed");

    // Send 3 requests — they should get ids 1, 2, 3
    int64_t id1 = t.send_request("method/one",   {}, nullptr);
    int64_t id2 = t.send_request("method/two",   {}, nullptr);
    int64_t id3 = t.send_request("method/three", {}, nullptr);

    assert(id1 == 1 && "Test 7: first id should be 1");
    assert(id2 == 2 && "Test 7: second id should be 2");
    assert(id3 == 3 && "Test 7: third id should be 3");
    assert(t.pending_count() == 3 && "Test 7: pending count should be 3");
    assert(t.write_queue_size() == 3 && "Test 7: write queue should have 3 entries");

    t.kill();
    t.wait();
    std::cout << " PASS\n";
}

} // anonymous namespace

int main() {
    std::cout << "=== LspTransport Tests ===\n";

    try {
        test_1_request_response_roundtrip();
        test_2_write_queue_ordering();
        test_3_write_queue_partial_drain();
        test_4_pending_request_map();
        test_5_timeout_fires();
        test_6_notification_dispatch();
        test_7_multiple_requests_different_ids();

        std::cout << "\nAll tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        return 1;
    }
}
