#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace codetldr {

// Unix domain socket server for daemon IPC.
// Binds to a SOCK_STREAM AF_UNIX socket at the given path.
// Stale socket detection: if a socket file exists but no process is
// listening (ECONNREFUSED), the stale file is removed before binding.
class IpcServer {
public:
    IpcServer() = default;
    ~IpcServer() { cleanup(); }

    // Non-copyable, movable
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) noexcept;
    IpcServer& operator=(IpcServer&&) noexcept;

    // Bind to sock_path with stale socket detection.
    // Returns false if a live daemon is already listening at that path.
    // Throws std::runtime_error on other bind/listen errors.
    bool bind_or_die(const std::filesystem::path& sock_path);

    // File descriptor for integration into poll() set
    int server_fd() const { return server_fd_; }

    // Non-blocking accept; returns -1 if no client waiting
    int accept_client() const;

    // NDJSON framing: read until newline from client_fd
    std::string recv_message(int client_fd) const;

    // Send JSON object followed by newline to client_fd
    void send_message(int client_fd, const nlohmann::json& msg) const;

    // Unlink socket file and close server fd
    void cleanup();

private:
    int server_fd_ = -1;
    std::filesystem::path sock_path_;
};

} // namespace codetldr
