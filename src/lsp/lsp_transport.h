#pragma once
#include "lsp/lsp_process.h"
#include "lsp/lsp_framing.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace codetldr {

// LspTransport: top-level non-blocking LSP stdio transport.
//
// Composes LspProcess (child process management) and LspFraming (Content-Length
// message reassembly) into a complete send/receive transport with:
//   - Write queue: serializes outgoing messages for non-blocking poll() drain
//   - Pending request map: matches responses to originating requests by id
//   - Per-request timeouts: fires error callback after configurable deadline
//
// Designed for integration into a POSIX poll() event loop:
//   1. Add stdout_fd() to poll POLLIN — call poll_read() when readable
//   2. Add stdin_fd() to poll POLLOUT when write_queue non-empty — call drain_writes()
//   3. Call check_timeouts() periodically (e.g., every second)
class LspTransport {
public:
    using ResponseCallback = std::function<void(const nlohmann::json& result,
                                                const nlohmann::json& error)>;
    using NotificationCallback = std::function<void(const std::string& method,
                                                    const nlohmann::json& params)>;

    LspTransport() = default;
    ~LspTransport() = default;

    // Non-copyable
    LspTransport(const LspTransport&) = delete;
    LspTransport& operator=(const LspTransport&) = delete;

    // Spawn the LSP server process. Returns 0 on success, errno on failure.
    int spawn(const std::string& command, const std::vector<std::string>& args);

    // Send a JSON-RPC request. Callback invoked when response arrives or timeout fires.
    // Returns the request ID assigned (monotonically incrementing int64_t).
    int64_t send_request(const std::string& method, const nlohmann::json& params,
                         ResponseCallback cb);

    // Send a JSON-RPC notification (no response expected).
    void send_notification(const std::string& method, const nlohmann::json& params);

    // Set callback for server-initiated notifications (messages with "method" but no "id").
    void set_notification_handler(NotificationCallback cb);

    // Non-blocking: drain pending writes to stdin pipe.
    // Call from poll() loop when stdin_fd is writable.
    // Returns true when write queue is empty.
    bool drain_writes();

    // Non-blocking: read available data from stdout pipe, extract complete messages.
    // Dispatches responses to pending callbacks, notifications to notification handler.
    // Returns list of raw messages received (for logging/debugging).
    std::vector<nlohmann::json> poll_read();

    // Check for timed-out pending requests. Call periodically from event loop.
    // Invokes error callback with {"code": -32000, "message": "Request timed out"}
    // for expired entries and removes them from the pending map.
    void check_timeouts();

    // Access underlying process (for poll() fd integration)
    int stdout_fd() const;
    int stdin_fd() const;
    int stderr_fd() const;
    pid_t pid() const;
    bool is_running() const;

    // Teardown
    void kill();
    int wait();

    // Stats
    size_t write_queue_size() const;
    size_t pending_count() const;

    // Configurable timeout (default 30s)
    void set_timeout(std::chrono::seconds timeout);

private:
    LspProcess process_;
    LspFraming framing_;

    // Write queue: Content-Length framed messages ready to send
    struct QueueEntry {
        std::string data;    // Full "Content-Length: N\r\n\r\n{body}"
        size_t offset = 0;   // Bytes already written
    };
    std::deque<QueueEntry> write_queue_;

    // Pending request map
    struct PendingRequest {
        ResponseCallback callback;
        std::chrono::steady_clock::time_point deadline;
    };
    std::unordered_map<int64_t, PendingRequest> pending_;

    int64_t next_id_ = 1;
    NotificationCallback notification_handler_;
    std::chrono::seconds timeout_{30};
};

} // namespace codetldr
