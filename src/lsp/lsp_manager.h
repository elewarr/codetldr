#pragma once
#include "lsp/lsp_transport.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <poll.h>

namespace codetldr {

enum class LspServerState {
    kNotStarted,   // Language not detected or not yet spawned
    kStarting,     // spawn() called, waiting for handshake (Phase 25)
    kReady,        // Handshake complete (set by Phase 25)
    kIndexing,     // Server busy with workspace scanning
    kUnavailable,  // Circuit-breaker open: 5 crashes in 3 minutes
    kDegraded,     // Running but missing compile_commands.json (clangd)
};

std::string to_string(LspServerState s);

struct LspServerConfig {
    std::string command;                 // e.g. "/usr/bin/clangd"
    std::vector<std::string> args;       // e.g. {"--background-index"}
    std::vector<std::string> extensions; // e.g. {".cpp", ".h", ".c"}
};

class LspManager {
public:
    using Clock = std::chrono::steady_clock;

    LspManager() = default;
    ~LspManager();

    LspManager(const LspManager&) = delete;
    LspManager& operator=(const LspManager&) = delete;

    void register_language(const std::string& language, LspServerConfig config);
    void set_detected_languages(const std::vector<std::string>& languages);
    bool ensure_server(const std::string& language);
    LspTransport* get_transport(const std::string& language);

    // Set project root URI for LSP initialize params.
    // Must be called before ensure_server().
    void set_project_root(const std::filesystem::path& root);

    // Gate API for Phase 26+ callers: send action when server is ready.
    // Use this instead of get_transport()->send_request() directly to respect
    // handshake state.
    //   - If state is kReady/kIndexing/kDegraded: executes action immediately, returns true.
    //   - If state is kStarting: queues action to run when kReady is reached, returns true.
    //   - Otherwise (kNotStarted, kUnavailable, unknown language): returns false.
    bool send_when_ready(const std::string& language, std::function<void()> action);

    // Call before any textDocument/* query. Idempotent per session.
    // Sends textDocument/didOpen with full file content if not already opened this session.
    void ensure_document_open(const std::string& language, const std::filesystem::path& file_path);

    // Per-loop: check crashed processes, schedule restarts.
    // now parameter enables deterministic testing (defaults to steady_clock::now()).
    bool tick(Clock::time_point now = Clock::now());

    void append_pollfds(std::vector<pollfd>& fds) const;
    void dispatch_read(int fd);
    nlohmann::json status_json() const;
    void shutdown();
    void check_timeouts();

    static constexpr int kMaxCrashesInWindow = 5;
    static constexpr std::chrono::seconds kCrashWindow{180};
    static constexpr std::chrono::seconds kBackoffMin{1};
    static constexpr std::chrono::seconds kBackoffMax{60};

    // Pure computation — exposed for testability
    std::chrono::seconds backoff_for(int restart_count) const;

private:
    struct ServerEntry {
        LspServerConfig config;
        LspTransport transport;
        LspServerState state = LspServerState::kNotStarted;
        bool detected = false;

        std::deque<Clock::time_point> crash_times;
        Clock::time_point restart_after;
        int restart_count = 0;

        // Requests queued while state is kStarting, drained when kReady is reached
        std::vector<std::function<void()>> pending_requests;
        // Per-session tracking of opened URIs, cleared on crash/restart
        std::unordered_set<std::string> opened_uris;
    };

    bool try_spawn(ServerEntry& entry, const std::string& language);
    void handle_crash(ServerEntry& entry, const std::string& language, Clock::time_point now);

    // Build LSP initialize params per LSP 3.17 spec
    static nlohmann::json make_initialize_params(const std::string& language,
                                                  const std::filesystem::path& project_root);

    // Probe filesystem for compile_commands.json after clangd reaches kReady.
    // Sets entry state to kDegraded with warning if not found.
    void check_clangd_compile_db(ServerEntry& entry);

    // Probe filesystem for Cargo.toml after rust-analyzer reaches kReady.
    // Logs warning if not found but does NOT set kDegraded (rust-analyzer supports single-file mode).
    void check_cargo_toml(ServerEntry& entry);

    // Map file extension to LSP languageId string.
    static std::string language_id_for(const std::filesystem::path& path);

    // Construct "file://" URI from an absolute or relative path.
    static std::string file_uri(const std::filesystem::path& path);

    std::unordered_map<std::string, ServerEntry> servers_;
    std::unordered_map<int, std::string> fd_to_language_;
    std::filesystem::path project_root_;
};

} // namespace codetldr
