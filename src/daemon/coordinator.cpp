#include "daemon/coordinator.h"
#include "analysis/pipeline.h"
#include "common/logging.h"
#include "embedding/embedding_worker.h"
#include "embedding/model_manager.h"
#include "embedding/vector_store.h"
#include "lsp/lsp_manager.h"
#include "lsp/lsp_call_graph_resolver.h"
#include "lsp/lsp_dependency_resolver.h"
#include "lsp/lsp_call_hierarchy_resolver.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <spdlog/spdlog.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#ifdef __linux__
#include <fstream>
#endif

namespace {
// Global stop flag — set by signal handlers (SIGTERM/SIGINT)
std::atomic<bool> g_stop{false};

void signal_handler(int /*sig*/) {
    g_stop.store(true, std::memory_order_relaxed);
}

#ifdef __linux__
static void warn_inotify_limit() {
    std::ifstream f("/proc/sys/fs/inotify/max_user_watches");
    if (!f) return;
    int max_watches = 0;
    f >> max_watches;
    if (max_watches > 0 && max_watches < 8192) {
        spdlog::warn(
            "inotify max_user_watches={} is low — file watching may fail for large projects. "
            "Increase with: echo fs.inotify.max_user_watches=524288 | "
            "sudo tee /etc/sysctl.d/40-inotify.conf && sudo sysctl -p",
            max_watches);
    }
}
#endif

// Source file extensions to index during initial scan (mirrors FileWatcher::is_source_file())
static const std::unordered_set<std::string> kScanExtensions = {
    ".cpp", ".h", ".py", ".js", ".ts", ".java", ".kt", ".swift",
    ".m", ".rs", ".c", ".cc", ".cxx", ".hpp", ".tsx", ".jsx", ".go"
};
} // anonymous namespace

namespace codetldr {

Coordinator::Coordinator(const std::filesystem::path& project_root,
                         SQLite::Database& db,
                         const LanguageRegistry& registry,
                         const std::filesystem::path& sock_path,
                         std::chrono::seconds idle_timeout)
    : project_root_(project_root)
    , db_(db)
    , registry_(registry)
    , sock_path_(sock_path)
    , idle_timeout_(idle_timeout)
    , ignore_filter_(IgnoreFilter::from_project_root(project_root))
    , file_watcher_(
        // on_event: push path to thread-safe queue
        [this](std::string path) {
            std::lock_guard<std::mutex> lock(event_queue_mutex_);
            event_queue_.push_back(std::move(path));
        },
        // on_wakeup: write to wakeup pipe
        [this]() { notify_wakeup(); }
      )
    , debouncer_(std::chrono::milliseconds(2000))
    , last_activity_(Clock::now()) {

    // Create wakeup self-pipe
    if (::pipe(wakeup_pipe_) < 0) {
        throw std::runtime_error("Coordinator: pipe() failed: " +
                                 std::string(strerror(errno)));
    }

    // Make read end non-blocking
    int flags = ::fcntl(wakeup_pipe_[0], F_GETFL, 0);
    ::fcntl(wakeup_pipe_[0], F_SETFL, flags | O_NONBLOCK);

#ifdef __linux__
    warn_inotify_limit();
#endif

    // Initialize status
    current_status_.state       = DaemonState::kStarting;
    current_status_.pid         = ::getpid();
    current_status_.socket_path = sock_path_.string();

    // Set up StatusWriter
    std::filesystem::path status_path = project_root_ / ".codetldr" / "status.json";
    status_writer_ = std::make_unique<StatusWriter>(status_path);

    // Set up RequestRouter (pass db_ for SearchEngine and ContextBuilder)
    router_ = std::make_unique<RequestRouter>(*this, db_);
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

    // Ignore SIGPIPE — writing to a closed LSP stdin pipe must not kill daemon (LSP-08)
    struct sigaction sigpipe_sa{};
    sigpipe_sa.sa_handler = SIG_IGN;
    sigemptyset(&sigpipe_sa.sa_mask);
    ::sigaction(SIGPIPE, &sigpipe_sa, nullptr);

    // Bind IPC server
    if (!ipc_server_.bind_or_die(sock_path_)) {
        throw std::runtime_error("Coordinator: another daemon is already running at " +
                                 sock_path_.string());
    }

    // Wire ignore filter into file watcher before starting
    file_watcher_.set_ignore_filter(&ignore_filter_);

    // Start file watcher
    file_watcher_.start(project_root_);

    // Enter kInitialScan state before scanning
    current_status_.state = DaemonState::kInitialScan;
    current_status_.pid = ::getpid();
    try { status_writer_->write(current_status_); } catch (...) {}

    // Initial full-project scan: index all existing source files
    spdlog::info("Coordinator: starting initial scan of {}", project_root_.string());
    int scan_count = 0;
    std::unordered_set<std::string> detected_languages;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             project_root_, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file()) continue;

        const auto& path = it->path();
        std::string ext = path.extension().string();
        if (kScanExtensions.count(ext) == 0) continue;

        // Check ignore filter
        std::filesystem::path rel_path;
        try {
            rel_path = std::filesystem::relative(path, project_root_);
        } catch (...) {
            continue;
        }
        if (ignore_filter_.should_ignore(rel_path)) continue;

        // Analyze file
        try {
            auto result = analyze_file(db_, registry_, path);
            if (result.success) {
                scan_count++;
                current_status_.files_indexed = scan_count;
                // Track detected language for LSP server selection
                if (lsp_manager_) {
                    const auto* lang_entry = registry_.for_extension(ext);
                    if (lang_entry && !lang_entry->name.empty()) {
                        detected_languages.insert(lang_entry->name);
                    }
                }
            } else {
                spdlog::warn("Coordinator: initial scan failed for {}: {}", path.string(), result.error);
            }
        } catch (const std::exception& ex) {
            spdlog::error("Coordinator: initial scan exception for {}: {}", path.string(), ex.what());
        }
    }

    // WAL checkpoint after initial scan
    try { db_.exec("PRAGMA wal_checkpoint(PASSIVE)"); } catch (...) {}
    spdlog::info("Coordinator: initial scan complete, indexed {} files", scan_count);

    // Notify LspManager which languages are present in the project
    if (lsp_manager_ && !detected_languages.empty()) {
        std::vector<std::string> langs(detected_languages.begin(), detected_languages.end());
        lsp_manager_->set_detected_languages(langs);
    }
    current_status_.files_total = scan_count;

    // Update status to idle
    current_status_.state = DaemonState::kIdle;
    current_status_.pid   = ::getpid();
    try {
        status_writer_->write(current_status_);
    } catch (...) {
        // Non-fatal: log but continue
    }

    std::time_t start_time = std::time(nullptr);
    last_activity_ = Clock::now();

    // Main event loop
    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !g_stop.load(std::memory_order_relaxed)) {

        // Compute poll timeout: min of debouncer next timeout and 5s idle-check interval
        int poll_timeout_ms = 5000;
        int debouncer_timeout = debouncer_.next_timeout_ms();
        if (debouncer_timeout >= 0) {
            poll_timeout_ms = std::min(poll_timeout_ms, debouncer_timeout);
        }

        std::vector<pollfd> fds;
        fds.reserve(8);  // IPC + wakeup + up to 6 LSP servers
        fds.push_back({ipc_server_.server_fd(), POLLIN, 0});
        fds.push_back({wakeup_pipe_[0], POLLIN, 0});
        if (lsp_manager_) {
            lsp_manager_->append_pollfds(fds);
        }

        int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout_ms);

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
                // Update last_activity_ on IPC request
                last_activity_ = Clock::now();
            }
        }

        // Handle wakeup pipe: drain bytes, then process file events
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (::read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
                // Drain pipe bytes
            }
            // Drain event queue into debouncer
            process_file_events();
        }

        // Dispatch LSP server reads and lifecycle management
        if (lsp_manager_) {
            for (size_t i = 2; i < fds.size(); ++i) {
                if (fds[i].revents & (POLLIN | POLLHUP)) {
                    lsp_manager_->dispatch_read(fds[i].fd);
                }
            }
            lsp_manager_->tick();
            lsp_manager_->check_timeouts();
        }

        // After handling events, flush debouncer and analyze ready files
        auto ready_paths = debouncer_.flush_ready();
        if (!ready_paths.empty()) {
            current_status_.state = DaemonState::kIndexing;
            try {
                status_writer_->write(current_status_);
            } catch (...) {}

            for (const auto& path : ready_paths) {
                spdlog::info("Coordinator: re-indexing {}", path);
                try {
                    // Phase 26/27: Read old content_hash BEFORE analyze_file overwrites it
                    std::string old_hash;
                    if (lsp_resolver_ || lsp_dependency_resolver_ || lsp_call_hierarchy_resolver_) {
                        try {
                            SQLite::Statement q(db_,
                                "SELECT content_hash FROM files WHERE path = ?");
                            q.bind(1, path);
                            if (q.executeStep() && !q.getColumn(0).isNull()) {
                                old_hash = q.getColumn(0).getString();
                            }
                        } catch (...) {}
                    }

                    auto result = analyze_file(db_, registry_, std::filesystem::path(path));
                    if (result.success) {
                        spdlog::info("Coordinator: indexed {} ({} symbols, {} calls)",
                                     path, result.symbols_count, result.calls_count);
                        current_status_.files_indexed++;

                        // Phase 26/27: trigger LSP resolution if content changed
                        if ((lsp_resolver_ || lsp_dependency_resolver_ || lsp_call_hierarchy_resolver_) &&
                            !result.content_hash.empty() &&
                            (old_hash.empty() || old_hash != result.content_hash)) {
                            try {
                                SQLite::Statement q(db_,
                                    "SELECT id, language FROM files WHERE path = ?");
                                q.bind(1, path);
                                if (q.executeStep()) {
                                    int64_t file_id = q.getColumn(0).getInt64();
                                    std::string lang = q.getColumn(1).isNull()
                                        ? "" : q.getColumn(1).getString();
                                    if (!lang.empty()) {
                                        if (lsp_resolver_) {
                                            lsp_resolver_->resolve_file(
                                                std::filesystem::path(path), file_id, lang);
                                        }
                                        if (lsp_dependency_resolver_) {
                                            lsp_dependency_resolver_->resolve_dependencies(
                                                std::filesystem::path(path), file_id, lang);
                                        }
                                        if (lsp_call_hierarchy_resolver_) {
                                            lsp_call_hierarchy_resolver_->resolve_incoming_callers(
                                                std::filesystem::path(path), file_id, lang);
                                        }
                                    }
                                }
                            } catch (const std::exception& ex) {
                                spdlog::warn("Coordinator: LSP resolve failed for {}: {}",
                                             path, ex.what());
                            }
                        }
                    } else {
                        spdlog::warn("Coordinator: analysis failed for {}: {}", path, result.error);
                    }
                } catch (const std::exception& ex) {
                    spdlog::error("Coordinator: exception analyzing {}: {}", path, ex.what());
                }
            }

            // WAL checkpoint after each batch
            try {
                db_.exec("PRAGMA wal_checkpoint(PASSIVE)");
            } catch (...) {}

            // Update activity and status
            last_activity_ = Clock::now();
            current_status_.state = DaemonState::kIdle;
            try {
                status_writer_->write(current_status_);
            } catch (...) {}
        }

        // Check idle timeout
        auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - last_activity_);
        if (idle_duration >= idle_timeout_) {
            spdlog::info("Coordinator: idle timeout reached ({} seconds), shutting down",
                         idle_timeout_.count());
            break;
        }

        // Update uptime
        current_status_.uptime_seconds =
            static_cast<int>(std::time(nullptr) - start_time);
    }

    shutdown();
}

void Coordinator::set_lsp_resolver(std::unique_ptr<LspCallGraphResolver> resolver) {
    lsp_resolver_ = std::move(resolver);
}

void Coordinator::set_lsp_dependency_resolver(std::unique_ptr<LspDependencyResolver> resolver) {
    lsp_dependency_resolver_ = std::move(resolver);
}

void Coordinator::set_lsp_call_hierarchy_resolver(std::unique_ptr<LspCallHierarchyResolver> resolver) {
    lsp_call_hierarchy_resolver_ = std::move(resolver);
}

void Coordinator::process_file_events() {
    // Drain the thread-safe event queue under lock
    std::vector<std::string> local_events;
    {
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        local_events.swap(event_queue_);
    }
    // Feed each path into the debouncer
    for (const auto& path : local_events) {
        debouncer_.touch(path);
    }
}

void Coordinator::request_stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    notify_wakeup();
}

nlohmann::json Coordinator::get_language_support() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& lang : registry_.language_names()) {
        nlohmann::json entry;
        entry["language"]     = lang;
        entry["l1_ast"]       = true;   // Tree-sitter L1 always available
        entry["l2_call_graph"] = true;  // Approximate call graph via Tree-sitter
        entry["l3_cfg"]       = false;  // Control flow graph not yet implemented
        entry["l4_dfg"]       = false;  // Data flow graph not yet implemented
        entry["l5_pdg"]       = false;  // Program dependency graph not yet implemented
        arr.push_back(std::move(entry));
    }
    return arr;
}

nlohmann::json Coordinator::get_status_json() {
    nlohmann::json j;
    j["state"]            = to_string(current_status_.state);
    j["pid"]              = current_status_.pid;
    j["socket_path"]      = current_status_.socket_path;
    j["files_indexed"]    = current_status_.files_indexed;
    j["files_total"]      = current_status_.files_total;
    j["last_indexed_at"]  = current_status_.last_indexed_at;
    j["uptime_seconds"]   = current_status_.uptime_seconds;
    // File watcher state
    j["watcher_active"]   = (file_watcher_.is_active());
    // Language support matrix
    j["language_support"] = get_language_support();
    // LSP server status (Phase 24)
    if (lsp_manager_) {
        j["lsp_servers"] = lsp_manager_->status_json();
    }
    return j;
}

void Coordinator::notify_wakeup() {
    if (wakeup_pipe_[1] >= 0) {
        char byte = 1;
        ::write(wakeup_pipe_[1], &byte, 1);
    }
}

nlohmann::json Coordinator::get_embedding_stats_json() {
    nlohmann::json j;

    // --- Model info ---
    std::string model_name = "none";
    std::string model_status_str = "not_configured";
    if (model_manager_) {
        switch (model_manager_->status()) {
            case ModelStatus::loaded:             model_status_str = "loaded"; break;
            case ModelStatus::model_not_installed: model_status_str = "not_installed"; break;
            case ModelStatus::load_failed:        model_status_str = "load_failed"; break;
            case ModelStatus::not_configured:     model_status_str = "not_configured"; break;
        }
        model_name = "configured";
    }
    j["model_name"]   = model_name;
    j["model_status"] = model_status_str;

    // --- Latency metrics from EmbeddingWorker ring buffer ---
    if (embedding_worker_) {
        auto snap = embedding_worker_->stats().snapshot();
        j["latency_p50_ms"]            = snap.p50_ms;
        j["latency_p95_ms"]            = snap.p95_ms;
        j["latency_p99_ms"]            = snap.p99_ms;
        j["latency_avg_ms"]            = snap.avg_ms;
        j["throughput_chunks_per_sec"] = snap.throughput_chunks_per_sec;
        j["queue_depth"]               = snap.queue_depth;
        j["chunks_embedded_total"]     = snap.chunks_processed;
        j["sample_count"]              = snap.sample_count;
    } else {
        j["latency_p50_ms"]            = 0.0;
        j["latency_p95_ms"]            = 0.0;
        j["latency_p99_ms"]            = 0.0;
        j["latency_avg_ms"]            = 0.0;
        j["throughput_chunks_per_sec"] = 0.0;
        j["queue_depth"]               = 0;
        j["chunks_embedded_total"]     = 0;
        j["sample_count"]              = 0;
    }

    // --- FAISS index stats ---
    int64_t faiss_count = vector_store_ ? vector_store_->ntotal() : 0;
    j["faiss_vector_count"] = faiss_count;

    // --- SQLite embedded count ---
    int64_t sqlite_count = 0;
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(DISTINCT symbol_id) FROM embedded_files");
        if (q.executeStep()) {
            sqlite_count = q.getColumn(0).getInt64();
        }
    } catch (...) {}
    j["sqlite_embedded_count"] = sqlite_count;

    // --- Health checks ---
    std::string health = "ok";
    nlohmann::json degraded = nlohmann::json(nullptr);

    // OBS-03: INDEX_INCONSISTENT — flag if FAISS/SQLite counts diverge by >5%
    if (faiss_count > 0 || sqlite_count > 0) {
        int64_t delta = std::abs(faiss_count - sqlite_count);
        int64_t threshold = std::max(static_cast<int64_t>(10),
                                     sqlite_count > 0 ? sqlite_count * 5 / 100 : static_cast<int64_t>(10));
        if (delta > threshold) {
            health = "degraded";
            degraded = "INDEX_INCONSISTENT: faiss=" + std::to_string(faiss_count)
                     + " sqlite=" + std::to_string(sqlite_count)
                     + " (delta=" + std::to_string(delta) + ")";
        }
    }

    // OBS-04: EXECUTION_PROVIDER_FALLBACK — macOS only, only when p50 > 20ms and sample_count >= 10
#if defined(__APPLE__)
    if (health == "ok" && embedding_worker_) {
        auto snap = embedding_worker_->stats().snapshot();
        if (snap.sample_count >= 10 && snap.p50_ms > 20.0) {
            health = "degraded";
            degraded = "EXECUTION_PROVIDER_FALLBACK: p50=" + std::to_string(snap.p50_ms)
                     + "ms exceeds 20ms threshold — CoreML EP may have fallen back to CPU. "
                       "Remediation: ensure Xcode Command Line Tools are installed and "
                       "the model has been CoreML-compiled: codetldr model download --force";
        }
    }
#endif

    j["health"]   = health;
    j["degraded"] = degraded;

    return j;
}

void Coordinator::shutdown() {
    // Shut down LSP servers before closing IPC
    if (lsp_manager_) {
        spdlog::info("Coordinator: shutting down LSP servers");
        lsp_manager_->shutdown();
    }

    // Stop file watcher first
    file_watcher_.stop();

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
