#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace codetldr {

// Client-side Unix domain socket connector.
// Used by CLI commands and tests to communicate with the daemon.
class DaemonClient {
public:
    DaemonClient() = default;
    ~DaemonClient() { close(); }

    // Non-copyable
    DaemonClient(const DaemonClient&) = delete;
    DaemonClient& operator=(const DaemonClient&) = delete;

    // Connect to daemon socket at sock_path.
    // Returns true on success, false if connection refused / not found.
    bool connect(const std::filesystem::path& sock_path);

    // Send a JSON-RPC 2.0 call and return the full response object.
    // Params may be null (omitted) or a JSON object/array.
    nlohmann::json call(const std::string& method,
                        const nlohmann::json& params = nullptr);

    // Close the socket connection
    void close();

    bool is_connected() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    int next_id_ = 1;
};

} // namespace codetldr
