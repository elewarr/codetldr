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
    int handshake_timeout_s = 0;         // 0 = use transport default (30s); INFRA-03
    nlohmann::json extra_init_options;   // null by default; merged into initializationOptions
};

class LspManager {
public:
    using Clock = std::chrono::steady_clock;

    LspManager() = default;
    ~LspManager();

    LspManager(const LspManager&) = delete;
    LspManager& operator=(const LspManager&) = delete;

    void register_language(const std::string& language, LspServerConfig config);
    void register_unavailable_language(const std::string& language,
                                       const std::string& reason);
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

    // Build LSP initialize params per LSP 3.17 spec.
    // Public static for testability — Plan 02 tests call this directly.
    static nlohmann::json make_initialize_params(const std::string& language,
                                                  const std::filesystem::path& project_root,
                                                  const nlohmann::json& extra_init_options = {});

    static constexpr int kMaxCrashesInWindow = 5;
    static constexpr std::chrono::seconds kCrashWindow{180};
    static constexpr std::chrono::seconds kBackoffMin{1};
    static constexpr std::chrono::seconds kBackoffMax{60};

    // Pure computation — exposed for testability
    std::chrono::seconds backoff_for(int restart_count) const;

    // Returns true when all detected, non-unavailable backends have reached
    // kReady, kIndexing, or kDegraded. Used by Coordinator as cold-start drain gate.
    bool all_backends_ready() const;

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
        std::string unavailable_reason;  // Set by register_unavailable_language()
    };

    bool try_spawn(ServerEntry& entry, const std::string& language);
    void handle_crash(ServerEntry& entry, const std::string& language, Clock::time_point now);

    // Probe filesystem for compile_commands.json after clangd reaches kReady.
    // Sets entry state to kDegraded with warning if not found.
    void check_clangd_compile_db(ServerEntry& entry);

    // Probe filesystem for Cargo.toml after rust-analyzer reaches kReady.
    // Logs warning if not found but does NOT set kDegraded (rust-analyzer supports single-file mode).
    void check_cargo_toml(ServerEntry& entry);

    // Probe filesystem for go.mod after gopls reaches kReady.
    // Walks up from project_root_ to find nearest go.mod. Scans subtree for multi-module detection.
    // Logs warning if not found but does NOT set kDegraded (gopls supports GOPATH/single-file mode).
    void check_go_mod(ServerEntry& entry);

    // Probe filesystem for Kotlin build files after kotlin-language-server reaches kReady.
    // Checks for build.gradle, build.gradle.kts, or pom.xml at project root.
    // Logs warning if not found but does NOT set kDegraded.
    void check_kotlin_build(ServerEntry& entry);

    // Probe filesystem for Java build files after jdtls reaches kReady.
    // Checks for pom.xml, build.gradle, or build.gradle.kts at project root.
    // Logs warning if not found but does NOT set kDegraded (jdtls handles single-file Java).
    void check_java_build(ServerEntry& entry);

    // Probe filesystem for Gemfile after ruby-lsp reaches kReady.
    // Walks up from project_root_ to find nearest Gemfile.
    // Logs warning if not found but does NOT set kDegraded (ruby-lsp supports standalone .rb files).
    void check_ruby_gemfile(ServerEntry& entry);

    // Map file extension to LSP languageId string.
    static std::string language_id_for(const std::filesystem::path& path);

    // Construct "file://" URI from an absolute or relative path.
    static std::string file_uri(const std::filesystem::path& path);

    std::unordered_map<std::string, ServerEntry> servers_;
    std::unordered_map<int, std::string> fd_to_language_;
    std::filesystem::path project_root_;
};

} // namespace codetldr
