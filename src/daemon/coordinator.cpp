#include "daemon/coordinator.h"
#include "analysis/pipeline.h"
#include "common/logging.h"
#include "config/paths.h"
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
    ".m", ".rs", ".c", ".cc", ".cxx", ".hpp", ".tsx", ".jsx"
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

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    {
        namespace fs = std::filesystem;
        XdgPaths xdg = resolve_xdg_paths();
        fs::path cache_dir = xdg.cache_home / "models" / "CodeRankEmbed";
        fs::path model_path = cache_dir / "model_quantized.onnx";
        fs::path tokenizer_path = cache_dir / "tokenizer.json";

        model_manager_ = std::make_unique<ModelManager>(model_path, tokenizer_path);

        if (model_manager_->status() == ModelStatus::loaded) {
            fs::path faiss_path = project_root_ / ".codetldr" / "vectors.faiss";
            vector_store_ = std::make_unique<VectorStore>(
                VectorStore::open(faiss_path, ModelManager::kEmbeddingDim));
            embedding_worker_ = std::make_unique<EmbeddingWorker>(
                db_, project_root_, model_manager_.get(), vector_store_.get(), model_path);
            spdlog::info("Coordinator: semantic search enabled (model loaded)");
        } else {
            spdlog::info("Coordinator: semantic search disabled (model status: {})",
                         model_manager_->status() == ModelStatus::model_not_installed
                             ? "not installed" : "load failed");
        }
    }
#endif
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
#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
                if (embedding_worker_) {
                    embedding_worker_->enqueue(result.file_id);
                }
#endif
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
    current_status_.files_total = scan_count;

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    if (embedding_worker_) {
        // Check model fingerprint: if changed or first run, trigger full rebuild
        namespace fs = std::filesystem;
        XdgPaths xdg = resolve_xdg_paths();
        fs::path model_path = xdg.cache_home / "models" / "CodeRankEmbed" / "model_quantized.onnx";
        std::string current_fp = EmbeddingWorker::compute_model_fingerprint(model_path);
        if (!current_fp.empty()) {
            std::string stored_fp;
            try {
                SQLite::Statement q(db_, "SELECT value FROM metadata WHERE key = 'model_fingerprint'");
                if (q.executeStep()) {
                    stored_fp = q.getColumn(0).getString();
                }
            } catch (...) {
                // metadata table may not exist if migrations haven't run yet — safe to ignore
            }
            if (stored_fp != current_fp) {
                spdlog::info("Coordinator: model fingerprint changed ({} -> {}), scheduling full re-embedding",
                             stored_fp.empty() ? "none" : stored_fp, current_fp);
                embedding_worker_->enqueue_full_rebuild();
            } else {
                spdlog::info("Coordinator: model fingerprint matches, skipping full rebuild");
            }
        }
    }
#endif

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

        struct pollfd fds[2]{};
        fds[0].fd     = ipc_server_.server_fd();
        fds[0].events = POLLIN;
        fds[1].fd     = wakeup_pipe_[0];
        fds[1].events = POLLIN;

        int rc = ::poll(fds, 2, poll_timeout_ms);

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
                    auto result = analyze_file(db_, registry_, std::filesystem::path(path));
                    if (result.success) {
                        spdlog::info("Coordinator: indexed {} ({} symbols, {} calls)",
                                     path, result.symbols_count, result.calls_count);
                        current_status_.files_indexed++;
#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
                        if (embedding_worker_) {
                            embedding_worker_->enqueue(result.file_id);
                        }
#endif
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
    return j;
}

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
std::vector<std::pair<int64_t, float>>
Coordinator::semantic_search(const std::string& query, int k) const {
    if (!model_manager_ || model_manager_->status() != ModelStatus::loaded) {
        return {};
    }
    if (!vector_store_ || vector_store_->ntotal() == 0) {
        return {};
    }
    auto query_vec = model_manager_->embed(query, /*is_query=*/true);
    return vector_store_->search(query_vec, k);
}
#endif

void Coordinator::notify_wakeup() {
    if (wakeup_pipe_[1] >= 0) {
        char byte = 1;
        ::write(wakeup_pipe_[1], &byte, 1);
    }
}

void Coordinator::shutdown() {
#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    // Stop embedding worker FIRST — it may be reading from SQLite
    if (embedding_worker_) {
        spdlog::info("Coordinator: stopping embedding worker");
        embedding_worker_->stop();
    }
#endif

    // Stop file watcher
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
