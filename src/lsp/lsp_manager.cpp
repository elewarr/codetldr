#include "lsp/lsp_manager.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace codetldr {

std::string to_string(LspServerState s) {
    switch (s) {
        case LspServerState::kNotStarted:  return "not_started";
        case LspServerState::kStarting:    return "starting";
        case LspServerState::kReady:       return "ready";
        case LspServerState::kIndexing:    return "indexing";
        case LspServerState::kUnavailable: return "unavailable";
        case LspServerState::kDegraded:    return "degraded";
    }
    return "unknown";
}

LspManager::~LspManager() {
    shutdown();
}

void LspManager::register_language(const std::string& language, LspServerConfig config) {
    // ServerEntry contains LspTransport which is non-copyable, so we must
    // default-construct in-place via operator[] then assign fields.
    auto& entry = servers_[language];
    entry.config = std::move(config);
    entry.state = LspServerState::kNotStarted;
    entry.detected = false;
}

void LspManager::set_detected_languages(const std::vector<std::string>& languages) {
    // First, clear all detected flags
    for (auto& [lang, entry] : servers_) {
        entry.detected = false;
    }
    // Then set detected for each language in the input
    for (const auto& lang : languages) {
        auto it = servers_.find(lang);
        if (it != servers_.end()) {
            it->second.detected = true;
        }
    }
}

bool LspManager::ensure_server(const std::string& language) {
    auto it = servers_.find(language);
    if (it == servers_.end()) {
        return false;
    }
    ServerEntry& entry = it->second;

    if (!entry.detected) {
        return false;
    }

    if (entry.state == LspServerState::kUnavailable) {
        return false;
    }

    // Already running in some capacity
    if (entry.state == LspServerState::kStarting ||
        entry.state == LspServerState::kReady ||
        entry.state == LspServerState::kIndexing ||
        entry.state == LspServerState::kDegraded) {
        return true;
    }

    // kNotStarted + first start (restart_count == 0)
    if (entry.state == LspServerState::kNotStarted && entry.restart_count == 0) {
        return try_spawn(entry, language);
    }

    return false;
}

LspTransport* LspManager::get_transport(const std::string& language) {
    auto it = servers_.find(language);
    if (it == servers_.end()) {
        return nullptr;
    }
    ServerEntry& entry = it->second;
    if (entry.state == LspServerState::kStarting ||
        entry.state == LspServerState::kReady ||
        entry.state == LspServerState::kIndexing ||
        entry.state == LspServerState::kDegraded) {
        return &entry.transport;
    }
    return nullptr;
}

bool LspManager::try_spawn(ServerEntry& entry, const std::string& language) {
    int rc = entry.transport.spawn(entry.config.command, entry.config.args);
    if (rc != 0) {
        spdlog::error("LspManager: failed to spawn LSP server for '{}': error {}", language, rc);
        return false;
    }
    entry.state = LspServerState::kStarting;
    int fd = entry.transport.stdout_fd();
    if (fd >= 0) {
        fd_to_language_[fd] = language;
    }
    spdlog::info("LspManager: spawned LSP server for '{}' (pid={}, stdout_fd={})",
                 language, entry.transport.pid(), fd);
    return true;
}

void LspManager::handle_crash(ServerEntry& entry, const std::string& language,
                               Clock::time_point now) {
    // Add crash time
    entry.crash_times.push_back(now);

    // Evict crashes older than kCrashWindow from the front
    while (!entry.crash_times.empty() &&
           (now - entry.crash_times.front()) > kCrashWindow) {
        entry.crash_times.pop_front();
    }

    if (static_cast<int>(entry.crash_times.size()) >= kMaxCrashesInWindow) {
        entry.state = LspServerState::kUnavailable;
        spdlog::error("LspManager: circuit breaker tripped for '{}': {} crashes in {}s window",
                      language, entry.crash_times.size(),
                      kCrashWindow.count());
        return;
    }

    // Schedule restart with exponential backoff
    auto delay = backoff_for(entry.restart_count);
    entry.restart_after = now + delay;
    entry.restart_count++;
    entry.state = LspServerState::kNotStarted;
    spdlog::warn("LspManager: LSP server '{}' crashed (attempt {}), restarting in {}s",
                 language, entry.restart_count, delay.count());
}

std::chrono::seconds LspManager::backoff_for(int restart_count) const {
    // 1 << restart_count seconds, capped at 60
    long delay = 1L << restart_count;
    return std::chrono::seconds(std::min(delay, static_cast<long>(kBackoffMax.count())));
}

bool LspManager::tick(Clock::time_point now) {
    bool changed = false;

    for (auto& [language, entry] : servers_) {
        if (!entry.detected) {
            continue;
        }

        // Check for crashed running processes
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {

            if (!entry.transport.is_running()) {
                // Remove old fd from map
                int old_fd = entry.transport.stdout_fd();
                if (old_fd >= 0) {
                    fd_to_language_.erase(old_fd);
                }
                entry.transport.wait();
                handle_crash(entry, language, now);
                changed = true;
            }
        }

        // Schedule pending restarts
        if (entry.state == LspServerState::kNotStarted && entry.restart_count > 0) {
            if (now >= entry.restart_after) {
                if (try_spawn(entry, language)) {
                    changed = true;
                }
            }
        }
    }

    return changed;
}

void LspManager::append_pollfds(std::vector<pollfd>& fds) const {
    for (const auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            int fd = entry.transport.stdout_fd();
            if (fd >= 0) {
                fds.push_back({fd, POLLIN, 0});
            }
        }
    }
}

void LspManager::dispatch_read(int fd) {
    auto it = fd_to_language_.find(fd);
    if (it == fd_to_language_.end()) {
        return;
    }
    const std::string& language = it->second;
    auto sit = servers_.find(language);
    if (sit == servers_.end()) {
        return;
    }
    sit->second.transport.poll_read();
}

nlohmann::json LspManager::status_json() const {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [language, entry] : servers_) {
        if (!entry.detected) {
            continue;
        }
        result.push_back({
            {"language", language},
            {"state", to_string(entry.state)}
        });
    }
    return result;
}

void LspManager::shutdown() {
    for (auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            entry.transport.kill();
            entry.transport.wait();
        }
    }
}

void LspManager::check_timeouts() {
    for (auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            entry.transport.check_timeouts();
        }
    }
}

} // namespace codetldr
