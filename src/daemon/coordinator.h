#pragma once
#include "daemon/ipc_server.h"
#include "daemon/request_router.h"
#include "daemon/status.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <atomic>
#include <memory>

// Forward declarations
namespace SQLite { class Database; }
namespace codetldr { class LanguageRegistry; }

namespace codetldr {

// Central event loop managing IPC and future watcher/analysis integration.
// Uses poll() to multiplex: IPC server fd + wakeup pipe read fd.
// On each IPC request: accept -> recv -> dispatch via RequestRouter -> send -> close.
// Checks stop_requested_ and g_stop signal flag each loop iteration.
//
// Clean shutdown sequence:
//   - Close IPC server (unlinks socket)
//   - PRAGMA wal_checkpoint(PASSIVE) on SQLite
//   - Write final status.json with state=stopped
//   - Remove PID file
class Coordinator {
public:
    // project_root: path to project directory (must contain .codetldr/)
    // db:           open Database reference
    // registry:     initialized LanguageRegistry reference
    // sock_path:    full path to daemon.sock
    Coordinator(const std::filesystem::path& project_root,
                SQLite::Database& db,
                const LanguageRegistry& registry,
                const std::filesystem::path& sock_path);

    ~Coordinator();

    // Non-copyable, non-movable
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    // Main event loop. Blocks until stop_requested_ or SIGTERM/SIGINT received.
    // Installs signal handlers before entering loop.
    void run();

    // Request graceful stop. Sets stop_requested_ = true, writes to wakeup pipe.
    // Safe to call from any thread.
    void request_stop();

    // Return current daemon status as a JSON object (for get_status RPC).
    nlohmann::json get_status_json();

    // Wakeup pipe read fd: for external threads (watcher in Plan 02) to add
    // to their own poll() set to wake the coordinator loop.
    int wakeup_pipe_read_fd() const { return wakeup_pipe_[0]; }

    // Write 1 byte to wakeup pipe, waking poll() in the event loop.
    // Called from watcher thread in Plan 02.
    void notify_wakeup();

private:
    void shutdown();

    std::filesystem::path project_root_;
    SQLite::Database& db_;
    const LanguageRegistry& registry_;
    std::filesystem::path sock_path_;

    IpcServer ipc_server_;
    std::unique_ptr<RequestRouter> router_;
    std::unique_ptr<StatusWriter> status_writer_;

    int wakeup_pipe_[2] = {-1, -1};  // [0]=read, [1]=write
    std::atomic<bool> stop_requested_{false};
    DaemonStatus current_status_;
};

} // namespace codetldr
