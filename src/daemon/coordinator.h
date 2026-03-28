#pragma once
#include "daemon/ipc_server.h"
#include "daemon/request_router.h"
#include "daemon/status.h"
#include "watcher/file_watcher.h"
#include "watcher/ignore_filter.h"
#include "watcher/debouncer.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

// Forward declarations
namespace SQLite { class Database; }
namespace codetldr {
    class LanguageRegistry;
    class EmbeddingWorker;
    class VectorStore;
    class ModelManager;
    class LspManager;
    class LspCallGraphResolver;
    class LspDependencyResolver;
    class LspCallHierarchyResolver;
}

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
    Coordinator(const std::filesystem::path& project_root,
                SQLite::Database& db,
                const LanguageRegistry& registry,
                const std::filesystem::path& sock_path,
                std::chrono::seconds idle_timeout = std::chrono::seconds(1800));

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

    // Return embedding pipeline stats as JSON (for get_embedding_stats RPC).
    // Computes health checks: INDEX_INCONSISTENT (OBS-03), EXECUTION_PROVIDER_FALLBACK (OBS-04).
    nlohmann::json get_embedding_stats_json();

    // Return per-language capability matrix (for get_project_overview and get_status RPC).
    nlohmann::json get_language_support() const;

    // Wakeup pipe read fd: for external threads (watcher) to add
    // to their own poll() set to wake the coordinator loop.
    int wakeup_pipe_read_fd() const { return wakeup_pipe_[0]; }

    // Write 1 byte to wakeup pipe, waking poll() in the event loop.
    // Called from watcher thread.
    void notify_wakeup();

    // Inject LSP lifecycle manager (non-owning). Call before run(). (Phase 24)
    // Also wires the LspManager into RequestRouter for workspace/symbol search (Phase 27).
    void set_lsp_manager(LspManager* mgr) {
        lsp_manager_ = mgr;
        if (router_) router_->set_lsp_manager(mgr);
    }

    // Inject LSP call graph resolver (owning). Call before run(). (Phase 26)
    void set_lsp_resolver(std::unique_ptr<LspCallGraphResolver> resolver);

    // Inject LSP dependency resolver (owning). Call before run(). (Phase 27)
    void set_lsp_dependency_resolver(std::unique_ptr<LspDependencyResolver> resolver);

    // Inject LSP call hierarchy resolver (owning). Call before run(). (Phase 27)
    void set_lsp_call_hierarchy_resolver(std::unique_ptr<LspCallHierarchyResolver> resolver);

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

    // Embedding pipeline handles — non-owning, may be null until Phase 15+ wires them
    EmbeddingWorker* embedding_worker_ = nullptr;
    VectorStore*     vector_store_     = nullptr;
    ModelManager*    model_manager_    = nullptr;

    // LSP lifecycle manager — non-owning, injected by main.cpp (Phase 24)
    LspManager* lsp_manager_ = nullptr;

    // LSP call graph resolver — owning, injected by main.cpp (Phase 26)
    std::unique_ptr<LspCallGraphResolver> lsp_resolver_;

    // LSP dependency resolver — owning, injected by main.cpp (Phase 27)
    std::unique_ptr<LspDependencyResolver> lsp_dependency_resolver_;

    // LSP call hierarchy resolver — owning, injected by main.cpp (Phase 27)
    std::unique_ptr<LspCallHierarchyResolver> lsp_call_hierarchy_resolver_;

    // Cold-start LSP resolution queue — populated once after set_detected_languages(),
    // drained one file per language per tick() when all_backends_ready() (Phase 33)
    bool cold_start_queues_populated_ = false;
    bool cold_start_complete_ = false;
    std::unordered_map<std::string,
        std::deque<std::pair<std::string, int64_t>>> cold_start_queues_;
};


} // namespace codetldr
