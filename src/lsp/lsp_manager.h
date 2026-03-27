#pragma once
#include "lsp/lsp_transport.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
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
    };

    bool try_spawn(ServerEntry& entry, const std::string& language);
    void handle_crash(ServerEntry& entry, const std::string& language, Clock::time_point now);

    std::unordered_map<std::string, ServerEntry> servers_;
    std::unordered_map<int, std::string> fd_to_language_;
};

} // namespace codetldr
