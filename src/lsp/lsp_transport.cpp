#include "lsp/lsp_transport.h"
#include <unistd.h>
#include <cerrno>
#include <vector>

namespace codetldr {

int LspTransport::spawn(const std::string& command, const std::vector<std::string>& args) {
    return process_.spawn(command, args);
}

int64_t LspTransport::send_request(const std::string& method, const nlohmann::json& params,
                                   ResponseCallback cb) {
    int64_t id = next_id_++;

    // Build JSON-RPC 2.0 request
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["method"] = method;
    msg["params"] = params;

    // Encode and enqueue
    std::string encoded = LspFraming::encode(msg);
    write_queue_.push_back(QueueEntry{std::move(encoded), 0});

    // Register pending entry with deadline
    auto deadline = std::chrono::steady_clock::now() + timeout_;
    pending_[id] = PendingRequest{std::move(cb), deadline};

    return id;
}

void LspTransport::send_notification(const std::string& method, const nlohmann::json& params) {
    // Build JSON-RPC 2.0 notification (no "id" field)
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    msg["params"] = params;

    // Encode and enqueue
    std::string encoded = LspFraming::encode(msg);
    write_queue_.push_back(QueueEntry{std::move(encoded), 0});
}

void LspTransport::set_notification_handler(NotificationCallback cb) {
    notification_handler_ = std::move(cb);
}

bool LspTransport::drain_writes() {
    while (!write_queue_.empty()) {
        auto& front = write_queue_.front();
        const char* data = front.data.data() + front.offset;
        size_t remaining = front.data.size() - front.offset;

        ssize_t n = ::write(process_.stdin_fd(), data, remaining);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Pipe full — try again next poll
                return false;
            }
            // Other write error — log and return (caller should check is_running)
            return false;
        }

        front.offset += static_cast<size_t>(n);
        if (front.offset == front.data.size()) {
            write_queue_.pop_front();
        } else {
            // Partial write — try again next poll
            return false;
        }
    }
    return true; // queue empty
}

std::vector<nlohmann::json> LspTransport::poll_read() {
    char buf[65536]; // 64KB read buffer
    ssize_t n = ::read(process_.stdout_fd(), buf, sizeof(buf));
    if (n <= 0) {
        return {};
    }

    framing_.feed(buf, static_cast<size_t>(n));

    std::vector<nlohmann::json> messages;
    while (auto msg_opt = framing_.extract()) {
        nlohmann::json& msg = *msg_opt;
        messages.push_back(msg);

        // Check if this is a response (has "id" field but no "method")
        if (msg.contains("id") && !msg.contains("method")) {
            // Response message — match to pending request
            int64_t id = msg["id"].get<int64_t>();
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                if (msg.contains("result")) {
                    it->second.callback(msg["result"], nullptr);
                } else if (msg.contains("error")) {
                    it->second.callback(nullptr, msg["error"]);
                } else {
                    // Malformed response — invoke with nulls
                    it->second.callback(nullptr, nullptr);
                }
                pending_.erase(it);
            }
        } else if (msg.contains("method")) {
            // Server-initiated notification or request
            if (notification_handler_) {
                std::string method = msg["method"].get<std::string>();
                nlohmann::json params = msg.value("params", nlohmann::json::object());
                notification_handler_(method, params);
            }
        }
    }

    return messages;
}

void LspTransport::check_timeouts() {
    auto now = std::chrono::steady_clock::now();

    // Collect expired ids (don't erase while iterating)
    std::vector<int64_t> expired;
    for (auto& [id, req] : pending_) {
        if (now > req.deadline) {
            expired.push_back(id);
        }
    }

    // Fire timeout callbacks and remove from map
    for (int64_t id : expired) {
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            nlohmann::json error;
            error["code"] = -32000;
            error["message"] = "Request timed out";
            it->second.callback(nullptr, error);
            pending_.erase(it);
        }
    }
}

int LspTransport::stdout_fd() const { return process_.stdout_fd(); }
int LspTransport::stdin_fd() const { return process_.stdin_fd(); }
int LspTransport::stderr_fd() const { return process_.stderr_fd(); }
pid_t LspTransport::pid() const { return process_.pid(); }
bool LspTransport::is_running() const { return process_.is_running(); }

void LspTransport::kill() { process_.kill(); }
int LspTransport::wait() { return process_.wait(); }

size_t LspTransport::write_queue_size() const { return write_queue_.size(); }
size_t LspTransport::pending_count() const { return pending_.size(); }

void LspTransport::set_timeout(std::chrono::seconds timeout) { timeout_ = timeout; }

} // namespace codetldr
