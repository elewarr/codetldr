#include "daemon/coordinator.h"
#include "common/logging.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <filesystem>

namespace {
// Global stop flag — set by signal handlers (SIGTERM/SIGINT)
std::atomic<bool> g_stop{false};

void signal_handler(int /*sig*/) {
    g_stop.store(true, std::memory_order_relaxed);
}
} // anonymous namespace

namespace codetldr {

Coordinator::Coordinator(const std::filesystem::path& project_root,
                         SQLite::Database& db,
                         const LanguageRegistry& registry,
                         const std::filesystem::path& sock_path)
    : project_root_(project_root)
    , db_(db)
    , registry_(registry)
    , sock_path_(sock_path) {

    // Create wakeup self-pipe
    if (::pipe(wakeup_pipe_) < 0) {
        throw std::runtime_error("Coordinator: pipe() failed: " +
                                 std::string(strerror(errno)));
    }

    // Make read end non-blocking
    int flags = ::fcntl(wakeup_pipe_[0], F_GETFL, 0);
    ::fcntl(wakeup_pipe_[0], F_SETFL, flags | O_NONBLOCK);

    // Initialize status
    current_status_.state       = DaemonState::kStarting;
    current_status_.pid         = ::getpid();
    current_status_.socket_path = sock_path_.string();

    // Set up StatusWriter
    std::filesystem::path status_path = project_root_ / ".codetldr" / "status.json";
    status_writer_ = std::make_unique<StatusWriter>(status_path);

    // Set up RequestRouter
    router_ = std::make_unique<RequestRouter>(*this);
}

Coordinator::~Coordinator() {
    if (wakeup_pipe_[0] >= 0) ::close(wakeup_pipe_[0]);
    if (wakeup_pipe_[1] >= 0) ::close(wakeup_pipe_[1]);
    wakeup_pipe_[0] = wakeup_pipe_[1] = -1;
}

void Coordinator::run() {
    // Install signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);

    // Ignore SIGHUP
    struct sigaction sigh{};
    sigh.sa_handler = SIG_IGN;
    sigemptyset(&sigh.sa_mask);
    ::sigaction(SIGHUP, &sigh, nullptr);

    // Bind IPC server
    if (!ipc_server_.bind_or_die(sock_path_)) {
        throw std::runtime_error("Coordinator: another daemon is already running at " +
                                 sock_path_.string());
    }

    // Update status to idle
    current_status_.state = DaemonState::kIdle;
    current_status_.pid   = ::getpid();
    try {
        status_writer_->write(current_status_);
    } catch (...) {
        // Non-fatal: log but continue
    }

    std::time_t start_time = std::time(nullptr);

    // Main event loop
    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !g_stop.load(std::memory_order_relaxed)) {

        struct pollfd fds[2]{};
        fds[0].fd     = ipc_server_.server_fd();
        fds[0].events = POLLIN;
        fds[1].fd     = wakeup_pipe_[0];
        fds[1].events = POLLIN;

        int rc = ::poll(fds, 2, 5000);  // 5s timeout for idle check

        if (rc < 0) {
            if (errno == EINTR) continue;  // Signal received — check stop flags
            break;
        }

        // Handle new IPC connections
        if (fds[0].revents & POLLIN) {
            int client_fd = ipc_server_.accept_client();
            if (client_fd >= 0) {
                std::string msg = ipc_server_.recv_message(client_fd);
                if (!msg.empty()) {
                    try {
                        nlohmann::json req = nlohmann::json::parse(msg);
                        nlohmann::json resp = router_->dispatch(req);
                        ipc_server_.send_message(client_fd, resp);
                    } catch (const std::exception& ex) {
                        // Malformed JSON — send parse error
                        nlohmann::json error_resp;
                        error_resp["jsonrpc"] = "2.0";
                        error_resp["id"]      = nullptr;
                        error_resp["error"]["code"]    = -32700;
                        error_resp["error"]["message"] = "Parse error";
                        ipc_server_.send_message(client_fd, error_resp);
                    }
                }
                ::close(client_fd);
            }
        }

        // Handle wakeup pipe (drain bytes)
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (::read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
                // Drain bytes — future: process watcher events here (Plan 02)
            }
        }

        // Update uptime
        current_status_.uptime_seconds =
            static_cast<int>(std::time(nullptr) - start_time);
    }

    shutdown();
}

void Coordinator::request_stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    notify_wakeup();
}

nlohmann::json Coordinator::get_status_json() {
    current_status_.uptime_seconds = 0;  // Will be updated by run() loop
    nlohmann::json j;
    j["state"]            = to_string(current_status_.state);
    j["pid"]              = current_status_.pid;
    j["socket_path"]      = current_status_.socket_path;
    j["files_indexed"]    = current_status_.files_indexed;
    j["files_total"]      = current_status_.files_total;
    j["last_indexed_at"]  = current_status_.last_indexed_at;
    j["uptime_seconds"]   = current_status_.uptime_seconds;
    return j;
}

void Coordinator::notify_wakeup() {
    if (wakeup_pipe_[1] >= 0) {
        char byte = 1;
        ::write(wakeup_pipe_[1], &byte, 1);
    }
}

void Coordinator::shutdown() {
    // Close IPC server (unlinks socket)
    ipc_server_.cleanup();

    // WAL checkpoint
    try {
        db_.exec("PRAGMA wal_checkpoint(PASSIVE)");
    } catch (...) {
        // Non-fatal
    }

    // Write final status
    current_status_.state = DaemonState::kStopped;
    try {
        status_writer_->write(current_status_);
    } catch (...) {
        // Non-fatal
    }

    // Remove PID file if it exists
    std::filesystem::path pidfile = project_root_ / ".codetldr" / "daemon.pid";
    try {
        std::filesystem::remove(pidfile);
    } catch (...) {
        // Non-fatal
    }
}

} // namespace codetldr
