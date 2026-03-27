#pragma once
#include "daemon/ipc_server.h"
#include "daemon/request_router.h"
#include "daemon/status.h"
#include "query/hybrid_search_engine.h"
#include "query/search_engine.h"
#include "watcher/file_watcher.h"
#include "watcher/ignore_filter.h"
#include "watcher/debouncer.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

// Forward declarations
namespace SQLite { class Database; }
namespace codetldr { class LanguageRegistry; }

namespace codetldr {

// Central event loop managing IPC, file system watching, and analysis integration.
// Uses poll() to multiplex: IPC server fd + wakeup pipe read fd.
// On each IPC request: accept -> recv -> dispatch via RequestRouter -> send -> close.
// On each wakeup: drain event queue -> debounce -> flush_ready -> analyze_file.
// Checks stop_requested_ and g_stop signal flag each loop iteration.
//
// Clean shutdown sequence:
//   - Stop file watcher
//   - Close IPC server (unlinks socket)
//   - PRAGMA wal_checkpoint(PASSIVE) on SQLite
//   - Write final status.json with state=stopped
//   - Remove PID file
class Coordinator {
public:
    // project_root:   path to project directory (must contain .codetldr/)
    // db:             open Database reference
    // registry:       initialized LanguageRegistry reference
    // sock_path:      full path to daemon.sock
    // idle_timeout:   auto-shutdown after this much idle time (default 30min)
    // hybrid_config:  RRF search tuning (parsed from config.toml [search] section)
    Coordinator(const std::filesystem::path& project_root,
                SQLite::Database& db,
                const LanguageRegistry& registry,
                const std::filesystem::path& sock_path,
                std::chrono::seconds idle_timeout = std::chrono::seconds(1800),
                HybridSearchConfig hybrid_config = {});

    ~Coordinator();

    // Non-copyable, non-movable
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    // Main event loop. Blocks until stop_requested_, SIGTERM/SIGINT, or idle timeout.
    // Installs signal handlers before entering loop.
    void run();

    // Request graceful stop. Sets stop_requested_ = true, writes to wakeup pipe.
    // Safe to call from any thread.
    void request_stop();

    // Return current daemon status as a JSON object (for get_status RPC).
    nlohmann::json get_status_json();

    // Return per-language capability matrix (for get_project_overview and get_status RPC).
    nlohmann::json get_language_support() const;

    // Semantic (FAISS) search — raw vector search path, separate from HybridSearchEngine.
    // Returns empty vector when CODETLDR_ENABLE_SEMANTIC_SEARCH is OFF or model not loaded.
    // When Phase 15 ModelManager is implemented, call model_manager_->embed(query, true).
    std::vector<SearchResult> semantic_search(const std::string& query, int limit = 20);

    // Wakeup pipe read fd: for external threads (watcher) to add
    // to their own poll() set to wake the coordinator loop.
    int wakeup_pipe_read_fd() const { return wakeup_pipe_[0]; }

    // Write 1 byte to wakeup pipe, waking poll() in the event loop.
    // Called from watcher thread.
    void notify_wakeup();

private:
    void shutdown();
    void process_file_events();

    std::filesystem::path project_root_;
    SQLite::Database& db_;
    const LanguageRegistry& registry_;
    std::filesystem::path sock_path_;
    std::chrono::seconds idle_timeout_;

    IpcServer ipc_server_;
    std::unique_ptr<RequestRouter> router_;
    std::unique_ptr<StatusWriter> status_writer_;

    // Ignore filter (loaded from .codetldrignore or defaults)
    IgnoreFilter ignore_filter_;

    // File watching + debouncing
    FileWatcher file_watcher_;
    Debouncer debouncer_;

    // Thread-safe event queue: watcher thread pushes, coordinator thread drains
    std::mutex event_queue_mutex_;
    std::vector<std::string> event_queue_;

    // Idle tracking
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_activity_;

    int wakeup_pipe_[2] = {-1, -1};  // [0]=read, [1]=write
    std::atomic<bool> stop_requested_{false};
    DaemonStatus current_status_;
};

} // namespace codetldr
